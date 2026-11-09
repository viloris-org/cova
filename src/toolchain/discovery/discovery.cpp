// Cova — system toolchain discovery (PRD §7.2 step 1).
#include "toolchain/discovery/discovery.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
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

bool all_digits(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return false;
    }
  }
  return true;
}

struct DriverMatch {
  std::string token;  // "g++", "clang++", ...
  std::string family; // "gcc", "clang", "clang-cl"
  std::string prefix; // "aarch64-linux-gnu" or empty for a host driver
};

// Longest tokens must be tried first so `clang-cl` is not mistaken for `clang`.
const std::pair<std::string_view, std::string_view> kDriverTokens[] = {
    {"clang-cl", "clang-cl"}, {"clang++", "clang"}, {"clang", "clang"}, {"g++", "gcc"},
    {"gcc", "gcc"},           {"c++", "gcc"},       {"cc", "gcc"},
};

std::optional<DriverMatch> parse_driver_name(std::string_view name) {
  for (const auto& [token, family] : kDriverTokens) {
    if (name == token) {
      return DriverMatch{std::string(token), std::string(family), {}};
    }
    if (name.size() > token.size() + 1 && str::ends_with(name, "-" + std::string(token))) {
      const std::string prefix(name.substr(0, name.size() - token.size() - 1));
      if (!prefix.empty() && !all_digits(prefix)) {
        return DriverMatch{std::string(token), std::string(family), prefix};
      }
    }
  }
  return std::nullopt;
}

// Accepts version suffixes (`g++-14`, `clang++-18`) and cross prefixes
// (`aarch64-linux-gnu-g++`) in any combination.
std::optional<DriverMatch> match_driver(std::string_view filename) {
  std::string name = str::to_lower(filename);
  if (str::ends_with(name, ".exe")) {
    name.resize(name.size() - 4);
  }
  if (auto parsed = parse_driver_name(name)) {
    return parsed;
  }
  // Strip one or more trailing "-<digits>" components.
  for (;;) {
    const auto dash = name.rfind('-');
    if (dash == std::string::npos || dash + 1 >= name.size()) {
      return std::nullopt;
    }
    if (!all_digits(std::string_view(name).substr(dash + 1))) {
      return std::nullopt;
    }
    name.resize(dash);
    if (auto parsed = parse_driver_name(name)) {
      return parsed;
    }
  }
}

bool is_cxx_driver(const DriverMatch& match) {
  // `clang`/`gcc` are C drivers: they do not link the C++ runtime, so using
  // them as the C++ driver silently breaks every `std::` program. Versioned and
  // prefixed forms keep the `++` in their token.
  return match.token == "clang++" || match.token == "g++" || match.token == "c++" ||
         match.token == "clang-cl";
}

std::optional<stdfs::path> find_first(const stdfs::path& directory,
                                      const std::vector<std::string>& names) {
  for (const auto& name : names) {
    // On Windows, sibling tools carry the .exe suffix; try it before falling
    // back to PATH so a managed toolchain never picks up an unrelated host tool.
    std::vector<std::string> candidates{name};
#ifdef _WIN32
    if (!str::ends_with(str::to_lower(name), ".exe")) {
      candidates.push_back(name + ".exe");
    }
#endif
    for (const std::string& candidate_name : candidates) {
      const auto candidate = directory / candidate_name;
      if (fs::is_regular_file(candidate)) {
        return candidate;
      }
    }
    if (auto path = fs::find_in_path(name)) {
      return path;
    }
  }
  return std::nullopt;
}

std::vector<stdfs::path> managed_compiler_candidates(const stdfs::path& root) {
  std::vector<stdfs::path> out;
  if (!fs::is_directory(root)) {
    return out;
  }
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(root, ec); it != stdfs::directory_iterator();
       it.increment(ec)) {
    if (!fs::is_directory(it->path())) {
      continue;
    }
    for (const auto& bin : {it->path() / "bin", it->path()}) {
      if (!fs::is_directory(bin)) {
        continue;
      }
      std::error_code scan_ec;
      for (auto entry = stdfs::directory_iterator(bin, scan_ec);
           entry != stdfs::directory_iterator(); entry.increment(scan_ec)) {
        if (!fs::is_regular_file(entry->path())) {
          continue;
        }
        auto match = match_driver(entry->path().filename().string());
        if (match && is_cxx_driver(*match)) {
          out.push_back(entry->path());
        }
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
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

} // namespace

std::optional<Version> parse_version_banner(std::string_view banner) {
  auto version = Version::extract(banner);
  if (!version) {
    return std::nullopt;
  }
  return *version;
}

std::optional<std::string> family_from_executable(std::string_view filename) {
  auto match = match_driver(filename);
  if (!match) {
    return std::nullopt;
  }
  return match->family;
}

std::optional<platform::Triple> target_from_executable(std::string_view filename) {
  auto match = match_driver(filename);
  if (!match || match->prefix.empty()) {
    return std::nullopt;
  }
  // The triple parser also understands MinGW spellings such as
  // `x86_64-w64-mingw32`. A prefix that does not parse means the driver does
  // not encode a target.
  auto parsed = platform::Triple::parse(match->prefix);
  if (!parsed) {
    return std::nullopt;
  }
  return *parsed;
}

SiblingTools find_sibling_tools(const stdfs::path& compiler_path, std::string_view family) {
  SiblingTools tools;
  const auto directory =
      compiler_path.has_parent_path() ? compiler_path.parent_path() : stdfs::path(".");
  const auto match = match_driver(compiler_path.filename().string());
  const std::string prefix = match ? match->prefix : std::string();
  auto prefixed = [&](std::string_view name) {
    return prefix.empty() ? std::string(name) : prefix + "-" + std::string(name);
  };

  if (family == "clang" || family == "clang-cl") {
    if (prefix.empty()) {
      if (auto tool = find_first(directory, {"ld.lld", "lld", "ld"})) {
        tools.linker = *tool;
      }
      if (auto tool = find_first(directory, {"llvm-ar", "ar"})) {
        tools.archiver = *tool;
      }
      if (auto tool = find_first(directory, {"llvm-objcopy", "objcopy"})) {
        tools.objcopy = *tool;
      }
    } else {
      // A prefixed cross driver invokes its own linker; recording the host `ld`
      // here would misrepresent the toolchain in `explain` and in fingerprints.
      if (auto tool = find_first(directory, {prefixed("ld.lld"), prefixed("ld")})) {
        tools.linker = *tool;
      }
      if (auto tool = find_first(directory, {prefixed("llvm-ar"), prefixed("ar")})) {
        tools.archiver = *tool;
      }
      if (auto tool = find_first(directory, {prefixed("llvm-objcopy"), prefixed("objcopy")})) {
        tools.objcopy = *tool;
      }
    }
  } else {
    if (prefix.empty()) {
      if (auto tool = find_first(directory, {"ld", "ld.lld"})) {
        tools.linker = *tool;
      }
      if (auto tool = find_first(directory, {"ar", "llvm-ar"})) {
        tools.archiver = *tool;
      }
      if (auto tool = find_first(directory, {"objcopy", "llvm-objcopy"})) {
        tools.objcopy = *tool;
      }
    } else {
      if (auto tool = find_first(directory, {prefixed("ld"), prefixed("ld.lld")})) {
        tools.linker = *tool;
      }
      if (auto tool = find_first(directory, {prefixed("ar"), prefixed("llvm-ar")})) {
        tools.archiver = *tool;
      }
      if (auto tool = find_first(directory, {prefixed("objcopy"), prefixed("llvm-objcopy")})) {
        tools.objcopy = *tool;
      }
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
    return fail<stdfs::path>(Error::make("cova::toolchain::resource_dir",
                                         "the compiler did not report a resource directory"));
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
  const stdfs::path candidate(path);
  // `/` is the host root, not a managed cross sysroot; treating it as one would
  // make every host compiler look self-contained.
  if (candidate == stdfs::path("/")) {
    return stdfs::path{};
  }
  return candidate;
}

Result<stdfs::path> query_program(const stdfs::path& compiler_path,
                                   std::string_view query) {
  proc::ProcessSpec spec;
  spec.program = compiler_path;
  spec.args = {std::string(query)};
  spec.timeout = std::chrono::milliseconds(3000);
  auto result = proc::run(spec);
  if (!result || !result->success()) {
    return fail<stdfs::path>(Error::make("cova::toolchain::subprogram",
                                         "the compiler did not answer a subprogram query"));
  }
  for (const auto& line : str::split(result->stdout_text, '\n')) {
    const std::string trimmed = str::trim(line);
    if (!trimmed.empty()) {
      return stdfs::path(trimmed);
    }
  }
  return fail<stdfs::path>(Error::make("cova::toolchain::subprogram",
                                       "the compiler reported no subprogram"));
}

stdfs::path resolve_reported_tool(const stdfs::path& compiler_directory,
                                  stdfs::path reported) {
  if (reported.empty()) {
    return {};
  }
  if (reported.is_absolute()) {
    return fs::is_regular_file(reported) ? reported : stdfs::path{};
  }
  if (reported.has_parent_path()) {
    const stdfs::path relative_to_compiler = compiler_directory / reported;
    if (fs::is_regular_file(relative_to_compiler)) {
      return relative_to_compiler;
    }
    return fs::is_regular_file(reported) ? reported : stdfs::path{};
  }
  std::vector<stdfs::path> candidates{compiler_directory / reported};
#ifdef _WIN32
  if (reported.extension().empty()) {
    candidates.push_back(compiler_directory / (reported.string() + ".exe"));
  }
#endif
  for (const auto& candidate : candidates) {
    if (fs::is_regular_file(candidate)) {
      return candidate;
    }
  }
  if (auto found = fs::find_in_path(reported.string())) {
    return *found;
  }
  return {};
}

void add_subprogram_component(ToolchainDescriptor& descriptor, const stdfs::path& compiler_path,
                              std::string role, std::string_view query) {
  if (descriptor.component(role) != nullptr) {
    return;
  }
  auto reported = query_program(compiler_path, query);
  if (!reported) {
    return;
  }
  const stdfs::path compiler_directory =
      compiler_path.has_parent_path() ? compiler_path.parent_path() : stdfs::path(".");
  const stdfs::path resolved = resolve_reported_tool(compiler_directory, *reported);
  if (resolved.empty()) {
    // GCC prints the literal name when it has no such file (for example
    // `-print-file-name=specs` on clang); that is not a tool identity.
    return;
  }
  ToolComponent component;
  component.role = std::move(role);
  component.path = fs::canonical_for_compare(resolved);
  component.sha256 = Sha256::hex_of_file(component.path).value_or("");
  descriptor.components.push_back(std::move(component));
}

// Driver subprograms are real inputs: replacing `as`, `ld`, `cc1plus`,
// `collect2`, `lto-wrapper` or a specs file changes the produced artifact even
// when the driver executable is byte-identical.
void add_compiler_subprogram_components(ToolchainDescriptor& descriptor,
                                        const stdfs::path& compiler_path) {
  static constexpr std::pair<std::string_view, std::string_view> kQueries[] = {
      {"cc1", "-print-prog-name=cc1"},
      {"cc1plus", "-print-prog-name=cc1plus"},
      {"assembler", "-print-prog-name=as"},
      {"collect2", "-print-prog-name=collect2"},
      {"lto-wrapper", "-print-prog-name=lto-wrapper"},
      {"specs", "-print-file-name=specs"},
  };
  for (const auto& [role, query] : kQueries) {
    add_subprogram_component(descriptor, compiler_path, std::string(role), query);
  }
  add_subprogram_component(descriptor, compiler_path, "linker", "-print-prog-name=ld");
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
    return fail<ToolchainDescriptor>(
        Error::make("cova::toolchain::not_executable",
                    "`" + fs::generic_string(compiler_path) + "` is not an executable compiler"));
  }
  const std::string filename = basename_lower(compiler_path);
  const auto match = match_driver(filename);
  if (!match) {
    return fail<ToolchainDescriptor>(
        Error::make("cova::toolchain::unknown_family",
                    "`" + filename + "` is not a recognised clang or gcc driver"));
  }
  const std::string& family = match->family;

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
    return fail<ToolchainDescriptor>(
        Error::make("cova::toolchain::version_probe", "could not determine the version of `" +
                                                          fs::generic_string(compiler_path) + "`")
            .with("output: " + str::collapse_whitespace(banner)));
  }

  const platform::Triple host = platform::host_triple();
  const auto inferred = target_from_executable(filename);
  const platform::Triple configured_target = inferred.value_or(host);

  ToolchainDescriptor descriptor;
  descriptor.family = family;
  descriptor.version = *version;
  descriptor.source = source;
  descriptor.host = host;
  descriptor.crt = configured_target.crt();
  descriptor.libcpp = platform::default_libcpp_for(descriptor.family, configured_target);
  descriptor.abi = configured_target.abi();
  descriptor.id =
      source_prefix(source) + "-" + descriptor.family + "-" + descriptor.version.to_string() + "-" +
      (inferred ? inferred->to_short_string() + "-" : "") +
      std::to_string(fnv1a64(fs::generic_string(fs::canonical_for_compare(compiler_path))) %
                     0xffffff);
  descriptor.trust_level = source == ToolchainSource::System ? "system" : "managed";
  descriptor.supported_targets = {configured_target.to_string()};
  if (inferred) {
    descriptor.supported_targets.push_back(inferred->to_short_string());
  } else {
    descriptor.supported_targets.push_back(host.to_short_string());
  }
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

  std::vector<std::string> cc_names;
  if (!match->prefix.empty()) {
    // Never fall back to the host `gcc`/`clang` for a prefixed cross driver: a C
    // source compiled by the host compiler would silently produce a host object
    // inside a cross build.
    if (descriptor.family == "clang") {
      cc_names.push_back(match->prefix + "-clang");
    } else if (descriptor.family == "clang-cl") {
      cc_names.push_back(match->prefix + "-clang-cl");
    } else {
      cc_names.push_back(match->prefix + "-gcc");
    }
  } else {
    cc_names.push_back(descriptor.family == "clang" ? "clang" : "gcc");
  }
  if (auto cc = find_first(compiler_path.parent_path(), cc_names)) {
    ToolComponent component;
    component.role = "cc";
    component.path = fs::canonical_for_compare(*cc);
    descriptor.components.push_back(std::move(component));
  }

  auto siblings = find_sibling_tools(compiler_path, descriptor.family);
  for (auto [role, path] :
       std::vector<std::pair<std::string, stdfs::path>>{{"linker", siblings.linker},
                                                        {"archiver", siblings.archiver},
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
    if (fs::is_directory(*default_sysroot)) {
      descriptor.default_sysroot = *default_sysroot;
    }
  }

  // A prefixed cross GCC carries the target C runtime and libstdc++. A cross
  // Clang is only self-contained when it was configured with a sysroot.
  if (inferred && *inferred != host) {
    descriptor.self_contained = descriptor.family == "gcc" || !descriptor.default_sysroot.empty();
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

void enrich_compiler_subprograms(ToolchainDescriptor& descriptor) {
  stdfs::path compiler = descriptor.component_path("cxx");
  if (compiler.empty()) {
    compiler = descriptor.component_path("cc");
  }
  if (compiler.empty()) {
    return;
  }
  add_compiler_subprogram_components(descriptor, compiler);
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
      roots.push_back(options.managed_root.empty() ? dirs->toolchains : options.managed_root);
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
      if (!fs::is_regular_file(it->path())) {
        continue;
      }
      const std::string name = basename_lower(it->path());
      const auto match = match_driver(name);
      if (!match || !is_cxx_driver(*match)) {
        continue;
      }
      const std::string canonical = fs::generic_string(fs::canonical_for_compare(it->path()));
      if (!seen.insert(canonical).second) {
        continue;
      }
      auto descriptor = describe_compiler(it->path(), ToolchainSource::System);
      if (!descriptor) {
        continue;
      }
      if (!options.families.empty() && std::find(options.families.begin(), options.families.end(),
                                                 descriptor->family) == options.families.end()) {
        continue;
      }
      descriptors.push_back(std::move(*descriptor));
    }
  }

  // Managed toolchains live one directory deeper (`<root>/<id>/bin/<driver>`),
  // so they need their own scan rather than the flat directory loop above.
  if (options.include_managed) {
    auto dirs = fs::cova_dirs();
    const stdfs::path managed_root = options.managed_root.empty()
                                         ? (dirs ? dirs->toolchains : stdfs::path{})
                                         : options.managed_root;
    if (!managed_root.empty()) {
      auto managed = discover_managed(managed_root);
      if (managed) {
        for (auto& descriptor : *managed) {
          const stdfs::path cxx = descriptor.component_path("cxx");
          const std::string canonical = fs::generic_string(fs::canonical_for_compare(cxx));
          if (!seen.insert(canonical).second) {
            continue;
          }
          if (!options.families.empty() &&
              std::find(options.families.begin(), options.families.end(), descriptor.family) ==
                  options.families.end()) {
            continue;
          }
          descriptors.push_back(std::move(descriptor));
        }
      }
    }
  }

  std::sort(descriptors.begin(), descriptors.end(),
            [](const ToolchainDescriptor& a, const ToolchainDescriptor& b) {
              if (a.family != b.family) {
                return a.family < b.family;
              }
              if (a.version != b.version) {
                return b.version < a.version;
              }
              return a.id < b.id;
            });
  // Distribution aliases (`g++`, `c++`, `x86_64-pc-linux-gnu-g++`) point at the
  // same binary. Listing all of them makes diagnostics noisy and lets ranking
  // pick an arbitrary path; keep one descriptor per (family, version, target,
  // content) identity.
  std::set<std::string> identities;
  std::vector<ToolchainDescriptor> unique;
  for (auto& descriptor : descriptors) {
    std::string key = descriptor.family + "|" + descriptor.version.to_string() + "|" +
                      std::string(to_string(descriptor.source)) + "|";
    std::vector<std::string> targets = descriptor.supported_targets;
    std::sort(targets.begin(), targets.end());
    key += str::join(targets, ",");
    const auto* cxx = descriptor.component("cxx");
    if (cxx != nullptr) {
      key += "|" + cxx->sha256;
    }
    if (identities.insert(key).second) {
      unique.push_back(std::move(descriptor));
    }
  }
  return unique;
}

Result<std::vector<ToolchainDescriptor>> discover_managed(const stdfs::path& toolchains_root) {
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

} // namespace cova::toolchain
