// Cova — build planning: manifest + toolchain → Build Graph (PRD §9.1).
#include "build/planner.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "build/cache/cache.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"

namespace cova::build {
namespace {

bool has_glob_magic(std::string_view pattern) {
  return pattern.find('*') != std::string_view::npos ||
         pattern.find('?') != std::string_view::npos ||
         pattern.find('[') != std::string_view::npos;
}

bool is_c_source(const fs::Path& path) {
  const std::string extension = str::to_lower(path.extension().string());
  return extension == ".c" || extension == ".m";
}

bool is_cpp_source(const fs::Path& path) {
  const std::string extension = str::to_lower(path.extension().string());
  static const std::vector<std::string> extensions = {".cc",  ".cpp", ".cxx", ".c++",
                                                      ".mm",  ".ixx", ".cppm", ".ccm"};
  return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool is_compilable(const fs::Path& path) {
  return is_c_source(path) || is_cpp_source(path);
}

std::string relative_text(const fs::Path& path, const fs::Path& base) {
  std::error_code ec;
  auto relative = path.lexically_relative(base);
  if (ec || relative.empty()) {
    return fs::generic_string(path);
  }
  return fs::generic_string(relative);
}

bool excluded(const fs::Path& file, const fs::Path& root,
              const std::vector<std::string>& patterns) {
  const std::string relative = relative_text(file, root);
  for (const auto& pattern : patterns) {
    if (relative == pattern || str::starts_with(relative, pattern + "/")) {
      return true;
    }
    if (has_glob_magic(pattern)) {
      auto matches = fs::glob(root, pattern);
      if (matches) {
        for (const auto& match : *matches) {
          if (fs::canonical_for_compare(match) == fs::canonical_for_compare(file)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

std::vector<fs::Path> expand_sources(const fs::Path& root,
                                     const std::vector<std::string>& patterns,
                                     const std::vector<std::string>& excludes,
                                     const std::string& entry_point) {
  std::set<std::string> seen;
  std::vector<fs::Path> sources;
  auto add = [&](const fs::Path& file) {
    if (!is_compilable(file) || fs::is_directory(file)) {
      return;
    }
    if (excluded(file, root, excludes)) {
      return;
    }
    const std::string key = fs::generic_string(fs::canonical_for_compare(file));
    if (seen.insert(key).second) {
      sources.push_back(fs::absolute_normalized(file));
    }
  };
  for (const auto& pattern : patterns) {
    if (!has_glob_magic(pattern)) {
      const auto candidate = root / pattern;
      if (fs::is_directory(candidate)) {
        for (const auto& file : fs::walk_files(candidate)) {
          add(file);
        }
      } else {
        add(candidate);
      }
      continue;
    }
    auto matches = fs::glob(root, pattern);
    if (!matches) {
      continue;
    }
    for (const auto& match : *matches) {
      if (fs::is_directory(match)) {
        for (const auto& file : fs::walk_files(match)) {
          add(file);
        }
      } else {
        add(match);
      }
    }
  }
  if (!entry_point.empty()) {
    add(root / entry_point);
  }
  std::sort(sources.begin(), sources.end());
  return sources;
}

std::string standard_flag(const manifest::Manifest& manifest, bool c_source) {
  if (c_source) {
    return "-std=c17";
  }
  std::string edition = str::to_lower(manifest.package.edition);
  if (edition.rfind("c++", 0) == 0 || edition.rfind("gnu++", 0) == 0) {
    return "-std=" + edition;
  }
  return "-std=c++26";
}

void append(std::vector<std::string>& target, const std::vector<std::string>& values) {
  target.insert(target.end(), values.begin(), values.end());
}

std::vector<std::string> make_includes(const manifest::Manifest& manifest) {
  std::vector<std::string> flags;
  std::set<std::string> seen;
  for (const auto& directory : manifest.build.public_includes) {
    const std::string flag = "-I" + fs::generic_string(manifest.root / directory);
    if (seen.insert(flag).second) {
      flags.push_back(flag);
    }
  }
  for (const auto& directory : manifest.build.private_includes) {
    const std::string flag = "-I" + fs::generic_string(manifest.root / directory);
    if (seen.insert(flag).second) {
      flags.push_back(flag);
    }
  }
  return flags;
}

std::vector<std::string> make_definitions(const manifest::Manifest& manifest,
                                          const manifest::TargetOverride* target_override) {
  std::vector<std::string> flags;
  for (const auto& definition : manifest.build.public_definitions) {
    flags.push_back("-D" + definition);
  }
  for (const auto& definition : manifest.build.private_definitions) {
    flags.push_back("-D" + definition);
  }
  if (target_override != nullptr) {
    for (const auto& definition : target_override->definitions) {
      flags.push_back("-D" + definition);
    }
  }
  return flags;
}

const manifest::TargetOverride* target_override_for(const manifest::Manifest& manifest,
                                                    const platform::Triple& target) {
  auto it = manifest.target_overrides.find(target.to_string());
  return it == manifest.target_overrides.end() ? nullptr : &it->second;
}

std::string output_directory(const fs::Path& target_dir, const platform::Triple& target,
                             const std::string& profile) {
  return fs::generic_string(target_dir / target_component(target) / profile);
}

std::string artifact_name(const manifest::Manifest& manifest) {
  return manifest.package.name.empty() ? "package" : manifest.package.name;
}

std::string shared_library_suffix(const platform::Triple& target) {
  if (target.os == platform::Os::Windows) {
    return ".dll";
  }
  if (target.os == platform::Os::Macos || target.os == platform::Os::Ios) {
    return ".dylib";
  }
  return ".so";
}

// A Windows GNU program should not depend on the MinGW runtime DLLs.
//
// The static libstdc++ carries out-of-line definitions of the exception
// classes, while GCC 16's C++26 headers define the same constructors inline
// and emit them into every object. A PE image cannot hold both a COMDAT
// definition from an object and an archive member's definition of the same
// symbol, so keep the first definition, the way ELF resolves weak symbols.
// Older GCC headers never define these constructors inline.
void append_windows_static_runtime(std::vector<std::string>& argv,
                                   const platform::Triple& target,
                                   const toolchain::ResolvedToolchain& toolchain) {
  if (target.os == platform::Os::Windows && target.env == platform::Env::Gnu) {
    argv.push_back("-static-libstdc++");
    argv.push_back("-static-libgcc");
    if (toolchain.descriptor.family == "gcc" && toolchain.descriptor.version.major >= 16) {
      argv.push_back("-Wl,--allow-multiple-definition");
    }
  }
}

// `<print>` reaches the terminal through helpers that MinGW's libstdc++ ships
// in libstdc++exp, not libstdc++. The archive must follow the objects that
// reference it, so this is appended after them. The library is a GCC/libstdc++
// implementation detail: Clang on Windows GNU normally uses libc++ (MSYS2
// CLANG64/CLANGARM64, llvm-mingw), where asking for libstdc++exp fails the link.
void append_windows_experimental_library(std::vector<std::string>& argv,
                                         const platform::Triple& target,
                                         const toolchain::ResolvedToolchain& toolchain) {
  if (target.os == platform::Os::Windows && target.env == platform::Env::Gnu &&
      toolchain.descriptor.family == "gcc") {
    argv.push_back("-lstdc++exp");
  }
}

std::string compiler_program(const toolchain::ResolvedToolchain& toolchain, bool c_source) {
  const auto role = c_source ? "cc" : "cxx";
  const auto path = toolchain.descriptor.component_path(role);
  if (!path.empty()) {
    return fs::generic_string(path);
  }
  return fs::generic_string(toolchain.descriptor.component_path("cxx"));
}

std::vector<std::string> target_flags(const toolchain::ResolvedToolchain& toolchain,
                                      const platform::Triple& target) {
  std::vector<std::string> flags;
  if (toolchain.descriptor.family == "clang" || toolchain.descriptor.family == "clang-cl") {
    if (target != toolchain.descriptor.host && target != platform::host_triple()) {
      flags.push_back("--target=" + target.to_string());
    }
  }
  if (!toolchain.sysroot_path.empty()) {
    flags.push_back("--sysroot=" + fs::generic_string(toolchain.sysroot_path));
  }
  return flags;
}

bool add_depfile_inputs(Action& action, const fs::Path& depfile, const fs::Path& base) {
  auto parsed = Depfile::load(depfile, base);
  if (!parsed) {
    return false;
  }
  std::set<std::string> existing;
  for (const auto& input : action.inputs) {
    existing.insert(fs::generic_string(fs::canonical_for_compare(input.path)));
  }
  for (const auto& dependency : parsed->dependencies) {
    if (!fs::is_regular_file(dependency)) {
      continue;
    }
    const std::string key = fs::generic_string(fs::canonical_for_compare(dependency));
    if (existing.insert(key).second) {
      action.inputs.push_back(
          FileInput{dependency, "header", InputOrigin::Discovered});
    }
  }
  // A parsed depfile is the minimum evidence that the provider discovered the
  // full input closure. Without it the action may only run; it may not be
  // reused from a cache or an incremental record.
  return true;
}

// Adds a compile action for one source and returns the object output path.
ActionId add_compile_action(BuildGraph& graph, const manifest::Manifest& manifest,
                            const toolchain::ResolvedToolchain& toolchain,
                            const PlanOptions& options, const fs::Path& output_root,
                            const fs::Path& source, const std::string& package_label,
                            fs::Path& object_out) {
  const bool c_source = is_c_source(source);
  const std::string relative = relative_text(source, manifest.root);
  std::string object_name = relative;
  const std::string extension = fs::Path(object_name).extension().string();
  if (!extension.empty()) {
    object_name = object_name.substr(0, object_name.size() - extension.size());
  }
  object_name += toolchain.target.object_suffix();
  fs::Path object_directory = fs::Path(output_root) / "obj";
  if (!options.object_namespace.empty()) {
    object_directory /= options.object_namespace;
  }
  object_out = object_directory / object_name;
  const fs::Path depfile = fs::Path(object_out.string() + ".d");

  const auto* override = target_override_for(manifest, options.target);
  std::vector<std::string> argv;
  argv.push_back(compiler_program(toolchain, c_source));
  append(argv, target_flags(toolchain, options.target));
  argv.push_back(standard_flag(manifest, c_source));
  argv.push_back("-MMD");
  argv.push_back("-MF");
  argv.push_back(fs::generic_string(depfile));
  append(argv, make_includes(manifest));
  append(argv, options.dependency_include_flags);
  append(argv, make_definitions(manifest, override));
  append(argv, options.dependency_definition_flags);
  const auto& profile = manifest.profile(options.profile);
  append(argv, profile.cxx_flags());
  append(argv, manifest.build.compile_options);
  if (override != nullptr) {
    append(argv, override->compile_options);
  }
  argv.push_back("-fdiagnostics-color=" + std::string(options.use_color ? "always" : "never"));
  argv.push_back("-c");
  argv.push_back(fs::generic_string(source));
  argv.push_back("-o");
  argv.push_back(fs::generic_string(object_out));

  Action action;
  action.kind = ActionKind::Compile;
  action.label = "compile " + relative;
  action.package = package_label;
  action.argv = std::move(argv);
  action.working_dir = manifest.root;
  action.inputs.push_back(FileInput{source, "source"});
  action.outputs.push_back(FileOutput{object_out, true});
  action.input_closure_complete = add_depfile_inputs(action, depfile, manifest.root);
  return graph.add(std::move(action));
}

std::string link_library_flag(const std::string& library) {
  if (library.empty()) {
    return {};
  }
  if (library.front() == '-') {
    return library;
  }
  if (library.find('/') != std::string::npos || library.find('\\') != std::string::npos) {
    return library;
  }
  return "-l" + library;
}

}  // namespace

std::string target_component(const platform::Triple& target) {
  std::string text = target.to_short_string();
  for (char& character : text) {
    if (character == '/' || character == '\\') {
      character = '-';
    }
  }
  return text;
}

Result<PlanResult> plan_package(const manifest::Manifest& manifest,
                                const toolchain::ResolvedToolchain& toolchain,
                                const PlanOptions& options, const Path& target_dir) {
  PlanResult result;
  const std::string output_root = output_directory(target_dir, options.target, options.profile);
  const std::string package_label = manifest.package.name;
  const auto* override = target_override_for(manifest, options.target);

  auto sources = expand_sources(manifest.root, manifest.build.sources,
                                manifest.build.exclude, manifest.build.entry_point);
  if (sources.empty() && !manifest.build.source_dirs.empty()) {
    sources = expand_sources(manifest.root, manifest.build.source_dirs, manifest.build.exclude,
                             manifest.build.entry_point);
  }
  if (sources.empty()) {
    result.notes.push_back("no source files matched the manifest; nothing to build");
    return result;
  }

  std::vector<ActionId> object_actions;
  std::vector<fs::Path> objects;
  std::map<std::string, ActionId> source_actions;
  for (const auto& source : sources) {
    fs::Path object;
    const ActionId id = add_compile_action(result.graph, manifest, toolchain, options,
                                           output_root, source, package_label, object);
    object_actions.push_back(id);
    objects.push_back(std::move(object));
    source_actions[fs::generic_string(source)] = id;
  }

  // Decide the package artifact.
  const std::string name = artifact_name(manifest);
  fs::Path final_output;
  ActionId final_action;
  if (manifest.build.kind == manifest::PackageKind::StaticLibrary) {
    const std::string prefix = options.target.static_library_prefix();
    const std::string suffix = options.target.static_library_suffix();
    final_output = fs::Path(output_root) / "lib" / (prefix + name + suffix);
  } else if (manifest.build.kind == manifest::PackageKind::SharedLibrary) {
    const std::string prefix = options.target.static_library_prefix();
    final_output = fs::Path(output_root) / "lib" / (prefix + name + shared_library_suffix(options.target));
  } else {
    final_output = fs::Path(output_root) / "bin" / (name + options.target.executable_suffix());
  }

  {
    std::vector<std::string> argv;
    ActionKind kind = ActionKind::Link;
    if (manifest.build.kind == manifest::PackageKind::StaticLibrary) {
      kind = ActionKind::Archive;
      const auto archiver = toolchain.descriptor.component_path("archiver");
      argv.push_back(archiver.empty() ? "ar" : fs::generic_string(archiver));
      argv.push_back("rcs");
      argv.push_back(fs::generic_string(final_output));
      for (const auto& object : objects) {
        argv.push_back(fs::generic_string(object));
      }
    } else {
      argv.push_back(compiler_program(toolchain, false));
      append(argv, target_flags(toolchain, options.target));
      append_windows_static_runtime(argv, options.target, toolchain);
      if (manifest.build.kind == manifest::PackageKind::SharedLibrary) {
        argv.push_back("-shared");
        argv.push_back("-fPIC");
      }
      append(argv, manifest.profile(options.profile).ld_flags());
      for (const auto& object : objects) {
        argv.push_back(fs::generic_string(object));
      }
      for (const auto& artifact : options.dependency_artifacts) {
        argv.push_back(artifact);
      }
      append_windows_experimental_library(argv, options.target, toolchain);
      argv.push_back("-o");
      argv.push_back(fs::generic_string(final_output));
      if (override != nullptr) {
        append(argv, override->link_options);
      }
      append(argv, manifest.build.link_options);
      for (const auto& library : manifest.build.link_libraries) {
        const std::string flag = link_library_flag(library);
        if (!flag.empty()) {
          argv.push_back(flag);
        }
      }
      argv.push_back("-pthread");
    }
    Action action;
    action.kind = kind;
    action.label = (kind == ActionKind::Archive ? "archive " : "link ") + name;
    action.package = package_label;
    action.argv = std::move(argv);
    action.working_dir = manifest.root;
    for (std::size_t index = 0; index < objects.size(); ++index) {
      action.inputs.push_back(FileInput{objects[index], "object"});
      action.dependencies.push_back(object_actions[index]);
    }
    action.outputs.push_back(FileOutput{final_output, true});
    // Archive/link inputs are the object files produced by this graph, which
    // are themselves complete inputs, so no provider scan is needed.
    action.input_closure_complete = true;
    final_action = result.graph.add(std::move(action));
  }
  result.final_action = final_action;

  result.expected_outputs.push_back(final_output);
  if (manifest.build.kind == manifest::PackageKind::Executable) {
    result.runnable_binary = final_output;
  }

  // Test programs are compiled and linked from tests/**/*.cpp. Executable test
  // programs never reuse the package's main object.
  if (options.include_tests) {
    auto test_sources = expand_sources(manifest.root, manifest.test.sources,
                                       manifest.test.exclude, "");
    fs::Path main_object;
    if (!manifest.build.entry_point.empty()) {
      const auto key = fs::generic_string(
          fs::absolute_normalized(manifest.root / manifest.build.entry_point));
      auto it = source_actions.find(key);
      if (it != source_actions.end()) {
        const std::size_t index = static_cast<std::size_t>(
            std::find(object_actions.begin(), object_actions.end(), it->second) -
            object_actions.begin());
        if (index < objects.size()) {
          main_object = objects[index];
        }
      }
    } else if (manifest.build.kind == manifest::PackageKind::Executable) {
      for (const auto& source : sources) {
        if (source.stem() == "main") {
          auto it = source_actions.find(fs::generic_string(source));
          if (it != source_actions.end()) {
            const std::size_t index = static_cast<std::size_t>(
                std::find(object_actions.begin(), object_actions.end(), it->second) -
                object_actions.begin());
            if (index < objects.size()) {
              main_object = objects[index];
            }
          }
          break;
        }
      }
    }

    for (const auto& source : test_sources) {
      fs::Path test_object;
      const ActionId test_compile = add_compile_action(result.graph, manifest, toolchain, options,
                                                       output_root, source, package_label,
                                                       test_object);
      const fs::Path binary = fs::Path(output_root) / "bin" /
                              (source.stem().string() + options.target.executable_suffix());
      std::vector<std::string> argv;
      argv.push_back(compiler_program(toolchain, false));
      append(argv, target_flags(toolchain, options.target));
      append_windows_static_runtime(argv, options.target, toolchain);
      append(argv, manifest.profile(options.profile).ld_flags());
      argv.push_back(fs::generic_string(test_object));
      for (const auto& object : objects) {
        if (!main_object.empty() && object == main_object) {
          continue;
        }
        argv.push_back(fs::generic_string(object));
      }
      for (const auto& artifact : options.dependency_artifacts) {
        argv.push_back(artifact);
      }
      append_windows_experimental_library(argv, options.target, toolchain);
      argv.push_back("-o");
      argv.push_back(fs::generic_string(binary));
      if (override != nullptr) {
        append(argv, override->link_options);
      }
      append(argv, manifest.build.link_options);
      for (const auto& library : manifest.build.link_libraries) {
        const std::string flag = link_library_flag(library);
        if (!flag.empty()) {
          argv.push_back(flag);
        }
      }
      Action link;
      link.kind = ActionKind::Link;
      link.label = "link test " + source.stem().string();
      link.package = package_label;
      link.argv = std::move(argv);
      link.working_dir = manifest.root;
      link.inputs.push_back(FileInput{test_object, "object"});
      link.inputs.push_back(FileInput{source, "source"});
      link.dependencies.push_back(test_compile);
      for (const auto& object_action : object_actions) {
        link.dependencies.push_back(object_action);
      }
      link.outputs.push_back(FileOutput{binary, true});
      link.input_closure_complete = true;
      result.graph.add(std::move(link));
      result.test_binaries.push_back(binary);
    }
  }

  if (!result.test_binaries.empty() && result.expected_outputs.empty()) {
    result.expected_outputs = result.test_binaries;
  }
  (void)result.runnable_binary;
  return result;
}

Result<PlanResult> plan_workspace(const WorkspaceProject& project,
                                  const toolchain::ResolvedToolchain& toolchain,
                                  const PlanOptions& options, const Path& target_dir) {
  PlanResult combined;
  if (project.members.empty()) {
    combined.notes.push_back("the workspace has no buildable members");
    return combined;
  }

  std::map<std::string, const manifest::Manifest*, std::less<>> by_name;
  for (const auto& member : project.members) {
    if (member.package.name.empty()) {
      return fail<PlanResult>(Error::make(
          "cova::build::package_name", "a package in the build set has no `package.name`"));
    }
    if (!by_name.emplace(member.package.name, &member).second) {
      return fail<PlanResult>(Error::make(
          "cova::build::duplicate_package",
          "two packages named `" + member.package.name + "` are in the build set"));
    }
  }

  std::set<std::string, std::less<>> roots;
  if (project.roots.empty()) {
    for (const auto& [name, manifest] : by_name) {
      (void)manifest;
      roots.insert(name);
    }
  } else {
    for (const auto& name : project.roots) {
      if (!by_name.contains(name)) {
        return fail<PlanResult>(Error::make("cova::build::unknown_package",
                                            "no package named `" + name +
                                                "` is in the build set"));
      }
      roots.insert(name);
    }
  }

  // Direct dependency edges, resolved to package names. An unresolvable
  // declaration is reported by the dependency resolver before planning; here it
  // simply contributes no edge.
  std::map<std::string, std::vector<std::string>, std::less<>> direct;
  for (const auto& [name, manifest] : by_name) {
    std::vector<std::string> dependencies;
    for (const auto* spec : manifest->target_dependencies(options.target)) {
      if (spec->dev && !options.include_tests) {
        continue;
      }
      std::optional<std::string> resolved;
      if (spec->path) {
        const std::string wanted = fs::generic_string(fs::absolute_normalized(*spec->path));
        for (const auto& [candidate_name, candidate] : by_name) {
          if (fs::generic_string(fs::absolute_normalized(candidate->root)) == wanted) {
            resolved = candidate_name;
            break;
          }
        }
      }
      if (!resolved && by_name.contains(spec->name)) {
        resolved = spec->name;
      }
      if (resolved && *resolved != name) {
        dependencies.push_back(*resolved);
      }
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    direct[name] = std::move(dependencies);
  }

  // Transitively reachable packages and the closure each package needs.
  // The traversal doubles as cycle detection: a clear error beats a planner that
  // silently drops an edge.
  std::map<std::string, int, std::less<>> state;  // 0 unvisited, 1 visiting, 2 done
  std::vector<std::string> stack;
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>, std::less<>> transitive;
  std::function<Status(const std::string&)> visit = [&](const std::string& name) -> Status {
    int& current = state[name];
    if (current == 2) {
      return ok();
    }
    if (current == 1) {
      std::string chain;
      auto start = std::find(stack.begin(), stack.end(), name);
      for (auto it = start; it != stack.end(); ++it) {
        chain += *it;
        chain += " -> ";
      }
      chain += name;
      return fail(Error::make("cova::build::dependency_cycle",
                              "dependency cycle detected: " + chain)
                      .with("packages must form a directed acyclic graph"));
    }
    current = 1;
    stack.push_back(name);
    std::vector<std::string> collected;
    auto edge = direct.find(name);
    if (edge != direct.end()) {
      for (const auto& dependency : edge->second) {
        auto status = visit(dependency);
        if (!status) {
          return status;
        }
        auto nested = transitive.find(dependency);
        if (nested != transitive.end()) {
          collected.insert(collected.end(), nested->second.begin(), nested->second.end());
        }
        collected.push_back(dependency);
      }
    }
    std::vector<std::string> unique;
    std::set<std::string, std::less<>> seen;
    for (const auto& name_of_dependency : collected) {
      if (seen.insert(name_of_dependency).second) {
        unique.push_back(name_of_dependency);
      }
    }
    transitive[name] = std::move(unique);
    stack.pop_back();
    current = 2;
    order.push_back(name);
    return ok();
  };

  for (const auto& [name, manifest] : by_name) {
    (void)manifest;
    auto status = visit(name);
    if (!status) {
      return fail<PlanResult>(status.error());
    }
  }

  // Only packages reachable from a selected root are built.
  std::set<std::string, std::less<>> reachable;
  std::vector<std::string> pending(roots.begin(), roots.end());
  while (!pending.empty()) {
    const std::string name = pending.back();
    pending.pop_back();
    if (!reachable.insert(name).second) {
      continue;
    }
    auto edge = direct.find(name);
    if (edge != direct.end()) {
      for (const auto& dependency : edge->second) {
        pending.push_back(dependency);
      }
    }
  }

  // Plan dependency-first so a package's link line can name the exact artifacts
  // its dependencies produced.
  std::map<std::string, PlanResult, std::less<>> plans;
  for (const auto& name : order) {
    if (!reachable.contains(name)) {
      continue;
    }
    auto manifest_it = by_name.find(name);
    if (manifest_it == by_name.end()) {
      continue;
    }
    const manifest::Manifest& manifest = *manifest_it->second;

    PlanOptions member_options = options;
    member_options.object_namespace = name;
    member_options.include_tests = options.include_tests && roots.contains(name);
    std::set<std::string, std::less<>> seen_flags;
    std::set<std::string, std::less<>> seen_artifacts;
    auto closure = transitive.find(name);
    if (closure != transitive.end()) {
      for (const auto& dependency_name : closure->second) {
        auto dependency_it = by_name.find(dependency_name);
        if (dependency_it == by_name.end()) {
          continue;
        }
        const manifest::Manifest& dependency = *dependency_it->second;
        for (const auto& directory : dependency.build.public_includes) {
          const std::string flag = "-I" + fs::generic_string(dependency.root / directory);
          if (seen_flags.insert(flag).second) {
            member_options.dependency_include_flags.push_back(flag);
          }
        }
        for (const auto& definition : dependency.build.public_definitions) {
          const std::string flag = "-D" + definition;
          if (seen_flags.insert(flag).second) {
            member_options.dependency_definition_flags.push_back(flag);
          }
        }
      }
      // Static linkers resolve left to right, so a dependent library must be
      // named before the libraries it needs: reverse of the dependency-first
      // closure.
      for (auto it = closure->second.rbegin(); it != closure->second.rend(); ++it) {
        auto dependency_it = by_name.find(*it);
        if (dependency_it == by_name.end() ||
            dependency_it->second->build.kind == manifest::PackageKind::Executable) {
          continue;
        }
        auto planned = plans.find(*it);
        if (planned != plans.end() && !planned->second.expected_outputs.empty()) {
          const std::string artifact =
              fs::generic_string(planned->second.expected_outputs.front());
          if (seen_artifacts.insert(artifact).second) {
            member_options.dependency_artifacts.push_back(artifact);
          }
        }
      }
    }

    auto plan = plan_package(manifest, toolchain, member_options, target_dir);
    if (!plan) {
      return fail<PlanResult>(plan.error());
    }
    plans.emplace(name, std::move(*plan));
  }

  // Merge the per-package graphs with explicit id remapping.
  std::map<std::string, ActionId, std::less<>> final_actions;
  for (const auto& name : order) {
    if (!reachable.contains(name)) {
      continue;
    }
    auto plan_it = plans.find(name);
    if (plan_it == plans.end()) {
      continue;
    }
    const PlanResult& plan = plan_it->second;
    std::map<std::uint64_t, ActionId> local;
    for (const auto* action : plan.graph.actions_in_order()) {
      Action clone = *action;
      clone.dependencies.clear();
      for (const auto& dependency : action->dependencies) {
        auto mapped = local.find(dependency.value);
        if (mapped != local.end()) {
          clone.dependencies.push_back(mapped->second);
        }
      }
      const ActionId id = combined.graph.add(std::move(clone));
      local[action->id.value] = id;
    }
    if (plan.final_action) {
      auto mapped = local.find(plan.final_action->value);
      if (mapped != local.end()) {
        final_actions[name] = mapped->second;
      }
    }
    for (const auto& note : plan.notes) {
      combined.notes.push_back(name + ": " + note);
    }
  }

  // Every link (including test links) needs its package's direct dependencies
  // built first. Archive actions get the same edge for a deterministic order.
  for (const auto& name : order) {
    if (!reachable.contains(name)) {
      continue;
    }
    auto edge = direct.find(name);
    if (edge == direct.end()) {
      continue;
    }
    std::vector<ActionId> dependency_actions;
    for (const auto& dependency : edge->second) {
      auto mapped = final_actions.find(dependency);
      if (mapped != final_actions.end()) {
        dependency_actions.push_back(mapped->second);
      }
    }
    if (dependency_actions.empty()) {
      continue;
    }
    for (auto& action : combined.graph.actions()) {
      if (action.package != name) {
        continue;
      }
      if (action.kind != ActionKind::Link && action.kind != ActionKind::Archive) {
        continue;
      }
      std::set<std::uint64_t> existing;
      for (const auto& dependency : action.dependencies) {
        existing.insert(dependency.value);
      }
      for (const auto& dependency : dependency_actions) {
        if (existing.insert(dependency.value).second) {
          action.dependencies.push_back(dependency);
        }
      }
    }
  }

  // Only the requested roots contribute reported artifacts and runnable/test
  // programs; dependencies exist to serve them.
  for (const auto& name : order) {
    if (!reachable.contains(name) || !roots.contains(name)) {
      continue;
    }
    auto plan_it = plans.find(name);
    if (plan_it == plans.end()) {
      continue;
    }
    for (const auto& output : plan_it->second.expected_outputs) {
      combined.expected_outputs.push_back(output);
    }
    if (!combined.runnable_binary && plan_it->second.runnable_binary) {
      combined.runnable_binary = plan_it->second.runnable_binary;
    }
    combined.test_binaries.insert(combined.test_binaries.end(),
                                  plan_it->second.test_binaries.begin(),
                                  plan_it->second.test_binaries.end());
  }

  if (combined.graph.empty()) {
    combined.notes.push_back("the workspace has no buildable members");
  }
  std::sort(combined.expected_outputs.begin(), combined.expected_outputs.end());
  combined.expected_outputs.erase(
      std::unique(combined.expected_outputs.begin(), combined.expected_outputs.end()),
      combined.expected_outputs.end());
  return combined;
}

std::string compile_commands_json(const PlanResult& plan) {
  std::vector<const Action*> compiles;
  for (const auto* action : plan.graph.actions_in_order()) {
    if (action->kind == ActionKind::Compile) {
      compiles.push_back(action);
    }
  }
  auto source_of = [](const Action& action) -> std::string {
    for (const auto& input : action.inputs) {
      if (input.role == "source") {
        return fs::generic_string(input.path);
      }
    }
    return action.inputs.empty() ? std::string() : fs::generic_string(action.inputs.front().path);
  };
  std::sort(compiles.begin(), compiles.end(),
            [&](const Action* a, const Action* b) { return source_of(*a) < source_of(*b); });

  json::Value database = json::Value::array();
  for (const auto* action : compiles) {
    json::Value entry = json::Value::object();
    entry.set("directory", fs::generic_string(action->working_dir));
    entry.set("file", source_of(*action));
    if (!action->outputs.empty()) {
      entry.set("output", fs::generic_string(action->outputs.front().path));
    }
    json::Value arguments = json::Value::array();
    for (const auto& argument : action->argv) {
      arguments.push(argument);
    }
    entry.set("arguments", std::move(arguments));
    database.push(std::move(entry));
  }
  return database.dump(2) + "\n";
}

}  // namespace cova::build
