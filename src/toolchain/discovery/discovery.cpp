// Cova — system toolchain discovery (PRD §7.2 step 1).
#include "toolchain/discovery/discovery.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/process/process.hpp"

namespace cova::toolchain {
namespace {

std::string basename_lower(const stdfs::path& path) {
  return str::to_lower(path.filename().string());
}

bool is_compiler_driver(std::string_view filename) {
  const std::string name = str::to_lower(filename);
  return name == "clang++" || name == "clang" || name == "clang-cl" || name == "g++" ||
         name == "gcc" || name == "c++";
}

std::vector<stdfs::path> managed_compiler_candidates(const stdfs::path& root) {
  std::vector<stdfs::path> out;
  if (!fs::is_directory(root)) {
    return out;
  }
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(root, ec); it != stdfs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_directory()) {
      continue;
    }
    for (const auto& name : {"clang++", "g++"}) {
      const auto candidate = it->path() / "bin" / name;
      if (fs::is_regular_file(candidate)) {
        out.push_back(candidate);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<stdfs::path> find_tool(const stdfs::path& compiler_dir,
                                     std::initializer_list<std::string_view> names) {
  for (const auto name : names) {
    const auto candidate = compiler_dir / std::string(name);
    if (fs::is_regular_file(candidate)) {
      return candidate;
    }
    if (auto path = fs::find_in_path(name)) {
      return path;
    }
  }
  return std::nullopt;
}

std::string source_prefix(ToolchainSource source) {
  switch (source) {
    case ToolchainSource::System:
      return "system";
    case ToolchainSource::Managed:
      return "managed";
    case ToolchainSource::UserPath:
      return "user";
    case ToolchainSource::Registry:
      return "registry";
    case ToolchainSource::Unknown:
      break;
  }
  return "unknown";
}

}  // namespace

std::optional<Version> parse_version_banner(std::string_view banner) {
  auto version = Version::extract(banner);
  if (!version) {
    return std::nullopt;
  }
  return *version;
}

std::optional<std::string> family_from_executable(std::string_view filename) {
  const std::string name = str::to_lower(filename);
  if (name == "clang++" || name == "clang") {
    return std::string("clang");
  }
  if (name == "clang-cl") {
    return std::string("clang-cl");
  }
  if (name == "g++" || name == "gcc") {
    return std::string("gcc");
  }
  if (name == "c++") {
    return std::string("gcc");
  }
  return std::nullopt;
}

SiblingTools find_sibling_tools(const stdfs::path& compiler_path, std::string_view family) {
  SiblingTools tools;
  const auto directory = compiler_path.has_parent_path() ? compiler_path.parent_path()
                                                         : stdfs::path(".");
  if (family == "clang" || family == "clang-cl") {
    if (auto tool = find_tool(directory, {"ld.lld", "lld", "ld"})) {
      tools.linker = *tool;
    }
    if (auto tool = find_tool(directory, {"llvm-ar", "ar"})) {
      tools.archiver = *tool;
    }
    if (auto tool = find_tool(directory, {"llvm-objcopy", "objcopy"})) {
      tools.objcopy = *tool;
    }
  } else {
    if (auto tool = find_tool(directory, {"ld", "ld.lld"})) {
      tools.linker = *tool;
    }
    if (auto tool = find_tool(directory, {"ar", "llvm-ar"})) {
      tools.archiver = *tool;
    }
    if (auto tool = find_tool(directory, {"objcopy", "llvm-objcopy"})) {
      tools.objcopy = *tool;
    }
  }
  return tools;
}

Result<stdfs::path> query_resource_dir(const stdfs::path& compiler_path) {
  proc::ProcessSpec spec;
  spec.program = compiler_path;
  spec.args = {"-print-resource-dir"};
  spec.timeout = std::chrono::milliseconds(3000);
  auto result = proc::run(spec);
  if (!result) {
    return fail<stdfs::path>(result.error());
  }
  const std::string path = str::trim(result->stdout_text);
  if (path.empty()) {
    return fail<stdfs::path>(Error::make(
        "cova::toolchain::resource_dir", "the compiler did not report a resource directory"));
  }
  return stdfs::path(path);
}

Result<stdfs::path> query_default_sysroot(const stdfs::path& compiler_path) {
  proc::ProcessSpec spec;
  spec.program = compiler_path;
  spec.args = {"--print-sysroot"};
  spec.timeout = std::chrono::milliseconds(3000);
  auto result = proc::run(spec);
  if (!result) {
    return fail<stdfs::path>(result.error());
  }
  const std::string path = str::trim(result->stdout_text);
  if (path.empty()) {
    return stdfs::path{};
  }
  return stdfs::path(path);
}

std::vector<std::string> query_supported_targets(const stdfs::path& compiler_path) {
  proc::ProcessSpec spec;
  spec.program = compiler_path;
  spec.args = {"--print-targets"};
  spec.timeout = std::chrono::milliseconds(3000);
  auto result = proc::run(spec);
  if (!result) {
    return {};
  }
  std::vector<std::string> targets;
  for (const auto& line : str::split(result->stdout_text, '\n')) {
    const std::string trimmed = str::trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    // The output looks like "  aarch64     - AArch64 (little endian)".
    auto parts = str::split_whitespace_quoted(trimmed);
    if (parts.empty()) {
      continue;
    }
    if (platform::arch_from_string(parts.front())) {
      targets.push_back(parts.front());
    }
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

Result<ToolchainDescriptor> describe_compiler(const stdfs::path& compiler_path,
                                              ToolchainSource source) {
  if (!fs::is_regular_file(compiler_path) || !proc::is_executable(compiler_path)) {
    return fail<ToolchainDescriptor>(Error::make(
        "cova::toolchain::not_executable",
        "`" + fs::generic_string(compiler_path) + "` is not an executable compiler"));
  }
  const std::string filename = basename_lower(compiler_path);
  auto family = family_from_executable(filename);
  if (!family) {
    return fail<ToolchainDescriptor>(Error::make(
        "cova::toolchain::unknown_family",
        "`" + filename + "` is not a recognised clang or gcc driver"));
  }

  proc::ProcessSpec spec;
  spec.program = compiler_path;
  spec.args = {"--version"};
  spec.timeout = std::chrono::milliseconds(3000);
  auto result = proc::run(spec);
  if (!result) {
    return fail<ToolchainDescriptor>(result.error());
  }
  const std::string banner = result->stdout_text + "\n" + result->stderr_text;
  auto version = parse_version_banner(banner);
  if (!version) {
    return fail<ToolchainDescriptor>(Error::make(
        "cova::toolchain::version_probe",
        "could not determine the version of `" + fs::generic_string(compiler_path) + "`")
        .with("output: " + str::collapse_whitespace(banner)));
  }

  ToolchainDescriptor descriptor;
  descriptor.family = *family;
  descriptor.version = *version;
  descriptor.source = source;
  descriptor.host = platform::host_triple();
  descriptor.crt = descriptor.host.crt();
  descriptor.libcpp = platform::default_libcpp_for(descriptor.family, descriptor.host);
  descriptor.abi = descriptor.host.abi();
  descriptor.id = source_prefix(source) + "-" + descriptor.family + "-" +
                  descriptor.version.to_string() + "-" +
                  std::to_string(fnv1a64(fs::generic_string(fs::canonical_for_compare(compiler_path))) %
                                 0xffffff);
  descriptor.trust_level = source == ToolchainSource::System ? "system" : "managed";
  descriptor.supported_targets = {descriptor.host.to_string()};
  descriptor.supported_targets.push_back(descriptor.host.to_short_string());
  std::sort(descriptor.supported_targets.begin(), descriptor.supported_targets.end());
  descriptor.supported_targets.erase(
      std::unique(descriptor.supported_targets.begin(), descriptor.supported_targets.end()),
      descriptor.supported_targets.end());

  ToolComponent cxx;
  cxx.role = "cxx";
  // Keep the driver name: clang++ and clang behave differently when linking
  // C++ (one links the C++ runtime, the other does not). Canonicalising the
  // symlink would silently change the driver mode.
  cxx.path = fs::absolute_normalized(compiler_path);
  cxx.version_text = str::trim(banner);
  cxx.version = *version;
  cxx.sha256 = Sha256::hex_of_file(cxx.path).value_or("");
  descriptor.components.push_back(cxx);

  if (auto cc = find_tool(compiler_path.parent_path(), {"clang", "gcc"})) {
    ToolComponent component;
    component.role = "cc";
    component.path = fs::canonical_for_compare(*cc);
    descriptor.components.push_back(std::move(component));
  }

  auto siblings = find_sibling_tools(compiler_path, descriptor.family);
  for (auto [role, path] : std::vector<std::pair<std::string, stdfs::path>>{
           {"linker", siblings.linker}, {"archiver", siblings.archiver},
           {"objcopy", siblings.objcopy}}) {
    if (path.empty()) {
      continue;
    }
    ToolComponent component;
    component.role = role;
    component.path = fs::canonical_for_compare(path);
    component.sha256 = Sha256::hex_of_file(component.path).value_or("");
    descriptor.components.push_back(std::move(component));
  }

  if (auto resource_dir = query_resource_dir(compiler_path)) {
    descriptor.resource_dir = *resource_dir;
  }
  if (auto default_sysroot = query_default_sysroot(compiler_path)) {
    descriptor.default_sysroot = *default_sysroot;
  }

  descriptor.capabilities.c_compilation = true;
  descriptor.capabilities.preprocess = true;
  descriptor.capabilities.cpp_compilation = descriptor.family != "msvc";
  descriptor.capabilities.static_library = true;
  descriptor.capabilities.executable_link = true;
  descriptor.capabilities.stdlib_headers = true;
  descriptor.capabilities.debug_info = true;
  descriptor.content_hash = descriptor.compute_fingerprint();
  return descriptor;
}

Result<std::vector<ToolchainDescriptor>> discover(const DiscoveryOptions& options) {
  std::vector<stdfs::path> roots;
  if (options.include_user_path) {
    for (const auto& entry : options.extra_roots) {
      roots.push_back(entry);
    }
  }
  if (options.include_system) {
    for (const auto& entry : fs::path_entries()) {
      roots.push_back(entry);
    }
    roots.emplace_back("/usr/bin");
    roots.emplace_back("/usr/local/bin");
  }
  if (options.include_managed) {
    if (auto dirs = fs::cova_dirs()) {
      roots.push_back(dirs->toolchains);
    }
  }

  std::vector<ToolchainDescriptor> descriptors;
  std::set<std::string> seen;
  for (const auto& root : roots) {
    if (!fs::is_directory(root)) {
      continue;
    }
    std::error_code ec;
    for (auto it = stdfs::directory_iterator(root, ec); it != stdfs::directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file()) {
        continue;
      }
      const std::string name = basename_lower(it->path());
      if (!is_compiler_driver(name)) {
        continue;
      }
      // Prefer the C++ drivers; a bare `clang`/`gcc` is only used when no C++
      // driver for the same family exists.
      if (name == "clang" || name == "gcc" || name == "c++") {
        continue;
      }
      const std::string canonical =
          fs::generic_string(fs::canonical_for_compare(it->path()));
      if (!seen.insert(canonical).second) {
        continue;
      }
      auto descriptor = describe_compiler(it->path(), ToolchainSource::System);
      if (!descriptor) {
        continue;
      }
      if (!options.families.empty() &&
          std::find(options.families.begin(), options.families.end(), descriptor->family) ==
              options.families.end()) {
        continue;
      }
      descriptors.push_back(std::move(*descriptor));
    }
  }

  std::sort(descriptors.begin(), descriptors.end(),
            [](const ToolchainDescriptor& a, const ToolchainDescriptor& b) {
              if (a.family != b.family) {
                return a.family < b.family;
              }
              return b.version < a.version;
            });
  return descriptors;
}

Result<std::vector<ToolchainDescriptor>> discover_managed(
    const stdfs::path& toolchains_root) {
  std::vector<ToolchainDescriptor> out;
  for (const auto& path : managed_compiler_candidates(toolchains_root)) {
    auto descriptor = describe_compiler(path, ToolchainSource::Managed);
    if (!descriptor) {
      continue;
    }
    out.push_back(std::move(*descriptor));
  }
  return out;
}

}  // namespace cova::toolchain
