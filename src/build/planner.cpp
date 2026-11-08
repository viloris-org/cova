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

void add_depfile_inputs(Action& action, const fs::Path& depfile, const fs::Path& base) {
  auto parsed = Depfile::load(depfile, base);
  if (!parsed) {
    return;
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
      action.inputs.push_back(FileInput{dependency, "header"});
    }
  }
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
  object_out = fs::Path(output_root) / "obj" / object_name;
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
  append(argv, make_definitions(manifest, override));
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
  add_depfile_inputs(action, depfile, manifest.root);
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
      if (manifest.build.kind == manifest::PackageKind::SharedLibrary) {
        argv.push_back("-shared");
        argv.push_back("-fPIC");
      }
      append(argv, manifest.profile(options.profile).ld_flags());
      for (const auto& object : objects) {
        argv.push_back(fs::generic_string(object));
      }
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
    final_action = result.graph.add(std::move(action));
  }

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
      const fs::Path binary = fs::Path(output_root) / "bin" / source.stem();
      std::vector<std::string> argv;
      argv.push_back(compiler_program(toolchain, false));
      append(argv, target_flags(toolchain, options.target));
      append(argv, manifest.profile(options.profile).ld_flags());
      argv.push_back(fs::generic_string(test_object));
      for (const auto& object : objects) {
        if (!main_object.empty() && object == main_object) {
          continue;
        }
        argv.push_back(fs::generic_string(object));
      }
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
      result.graph.add(std::move(link));
      result.test_binaries.push_back(binary);
    }
  }

  if (!result.test_binaries.empty() && result.expected_outputs.empty()) {
    result.expected_outputs = result.test_binaries;
  }
  // Keep final_action referenced for clarity of the graph contract.
  (void)final_action;
  (void)result.runnable_binary;
  return result;
}

Result<PlanResult> plan_workspace(const WorkspaceProject& project,
                                  const toolchain::ResolvedToolchain& toolchain,
                                  const PlanOptions& options, const Path& target_dir) {
  PlanResult combined;
  std::map<std::string, Result<PlanResult>, std::less<>> per_member;
  for (const auto& member : project.members) {
    per_member.emplace(member.package.name,
                       plan_package(member, toolchain, options, target_dir));
  }

  std::map<std::string, std::pair<ActionId, std::string>, std::less<>> final_actions;
  // Add each member's actions with an explicit id remap.
  combined = PlanResult{};
  final_actions.clear();
  for (const auto& member : project.members) {
    auto it = per_member.find(member.package.name);
    if (it == per_member.end() || !it->second) {
      continue;
    }
    const auto& plan = *it->second;
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
    if (!plan.expected_outputs.empty()) {
      combined.expected_outputs.push_back(plan.expected_outputs.front());
    }
    if (plan.runnable_binary) {
      if (!combined.runnable_binary) {
        combined.runnable_binary = plan.runnable_binary;
      }
    }
    combined.test_binaries.insert(combined.test_binaries.end(), plan.test_binaries.begin(),
                                  plan.test_binaries.end());
    for (const auto& note : plan.notes) {
      combined.notes.push_back(member.package.name + ": " + note);
    }
    // Remember the member's final action so intra-workspace dependencies can be
    // wired after all members are present.
    if (!combined.graph.actions().empty()) {
      const auto& last = combined.graph.actions().back();
      final_actions[member.package.name] = {last.id, member.package.name};
    }
  }

  // Wire member dependencies: a dependency on another member contributes build
  // order, public include directories and the dependency's final artifact.
  std::map<std::string, const manifest::Manifest*, std::less<>> manifests;
  for (const auto& member : project.members) {
    manifests[member.package.name] = &member;
  }

  auto member_for_dependency = [&](const manifest::DependencySpec& dependency)
      -> std::optional<std::string> {
    if (dependency.path) {
      for (const auto& member : project.members) {
        if (fs::absolute_normalized(member.root) == fs::absolute_normalized(*dependency.path)) {
          return member.package.name;
        }
      }
    }
    if (manifests.contains(dependency.name)) {
      return dependency.name;
    }
    return std::nullopt;
  };

  auto collect_archives = [&](const std::string& root_name) {
    std::vector<std::string> archives;
    std::set<std::string> seen;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
      if (!seen.insert(name).second) {
        return;
      }
      auto manifest_it = manifests.find(name);
      if (manifest_it == manifests.end()) {
        return;
      }
      for (const auto& dependency : manifest_it->second->dependencies) {
        auto target = member_for_dependency(dependency);
        if (target && *target != root_name) {
          visit(*target);
        }
      }
      if (name == root_name) {
        return;
      }
      if (manifest_it->second->build.kind == manifest::PackageKind::Executable) {
        return;
      }
      auto plan_it = per_member.find(name);
      if (plan_it != per_member.end() && plan_it->second &&
          !plan_it->second->expected_outputs.empty()) {
        const std::string output = fs::generic_string(plan_it->second->expected_outputs.front());
        if (std::find(archives.begin(), archives.end(), output) == archives.end()) {
          archives.push_back(output);
        }
      }
    };
    visit(root_name);
    return archives;
  };

  for (const auto& member : project.members) {
    auto member_it = final_actions.find(member.package.name);
    if (member_it == final_actions.end()) {
      continue;
    }
    std::vector<std::string> include_flags;
    std::vector<std::string> definition_flags;
    std::set<std::string> seen_flags;
    for (const auto& dependency : member.dependencies) {
      auto target = member_for_dependency(dependency);
      if (!target) {
        continue;
      }
      auto dependency_manifest = manifests.find(*target);
      if (dependency_manifest != manifests.end()) {
        for (const auto& directory : dependency_manifest->second->build.public_includes) {
          const std::string flag = "-I" + fs::generic_string(dependency_manifest->second->root /
                                                             directory);
          if (seen_flags.insert(flag).second) {
            include_flags.push_back(flag);
          }
        }
        for (const auto& definition : dependency_manifest->second->build.public_definitions) {
          const std::string flag = "-D" + definition;
          if (seen_flags.insert(flag).second) {
            definition_flags.push_back(flag);
          }
        }
      }
      auto dependency_it = final_actions.find(*target);
      if (dependency_it != final_actions.end()) {
        for (auto* action : [&] {
               std::vector<Action*> links;
               for (auto& candidate : combined.graph.actions()) {
                 if (candidate.package == member.package.name &&
                     candidate.kind == ActionKind::Link) {
                   links.push_back(&candidate);
                 }
               }
               return links;
             }()) {
          action->dependencies.push_back(dependency_it->second.first);
        }
      }
      combined.notes.push_back(member.package.name + " depends on " + *target);
    }

    // Apply the dependency interface to every compile action of the member.
    for (auto& action : combined.graph.actions()) {
      if (action.package != member.package.name || action.kind != ActionKind::Compile) {
        continue;
      }
      for (const auto& flag : include_flags) {
        action.argv.insert(action.argv.end() - 4, flag);
      }
      for (const auto& flag : definition_flags) {
        action.argv.insert(action.argv.end() - 4, flag);
      }
    }

    // Attach dependency archives to link actions of the member.
    const auto archives = collect_archives(member.package.name);
    for (auto& action : combined.graph.actions()) {
      if (action.package != member.package.name || action.kind != ActionKind::Link) {
        continue;
      }
      auto output = std::find(action.argv.begin(), action.argv.end(), "-o");
      const auto position = output == action.argv.end() ? action.argv.end() : output;
      action.argv.insert(position, archives.begin(), archives.end());
    }
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

}  // namespace cova::build
