// Cova — manifest model (PRD §5.3).
#include "manifest/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"

namespace cova::manifest {
namespace {

using toml::Table;
using toml::Value;

std::optional<const Value*> lookup(const Table& table, std::string_view key) {
  auto it = table.find(key);
  if (it == table.end()) {
    return std::nullopt;
  }
  return &it->second;
}

Result<std::string> read_string(const Table& table, std::string_view key,
                                std::string_view context, std::string fallback = {}) {
  auto value = lookup(table, key);
  if (!value) {
    return fallback;
  }
  if (!(*value)->is_string() && (*value)->kind() != Value::Kind::Datetime) {
    return fail<std::string>(Error::make("cova::manifest::type",
                                         "expected a string for `" + std::string(key) + "` in " +
                                             std::string(context) + ", found " +
                                             (*value)->type_name()));
  }
  return (*value)->as_string();
}

Result<bool> read_bool(const Table& table, std::string_view key, std::string_view context,
                       bool fallback) {
  auto value = lookup(table, key);
  if (!value) {
    return fallback;
  }
  if (!(*value)->is_bool()) {
    return fail<bool>(Error::make("cova::manifest::type",
                                  "expected a boolean for `" + std::string(key) + "` in " +
                                      std::string(context) + ", found " +
                                      (*value)->type_name()));
  }
  return (*value)->as_bool();
}

Result<std::vector<std::string>> read_array(const Table& table, std::string_view key,
                                            std::string_view context) {
  auto value = lookup(table, key);
  if (!value) {
    return std::vector<std::string>{};
  }
  if ((*value)->is_string()) {
    // A single string is accepted as a one-element list.
    return std::vector<std::string>{(*value)->as_string()};
  }
  if (!(*value)->is_array()) {
    return fail<std::vector<std::string>>(
        Error::make("cova::manifest::type",
                    "expected an array of strings for `" + std::string(key) + "` in " +
                        std::string(context) + ", found " + (*value)->type_name()));
  }
  const auto* array = (*value)->as_array();
  std::vector<std::string> out;
  out.reserve(array->size());
  for (const auto& entry : *array) {
    if (!entry.is_string()) {
      return fail<std::vector<std::string>>(
          Error::make("cova::manifest::type",
                      "expected a string element in `" + std::string(key) + "` of " +
                          std::string(context) + ", found " + entry.type_name()));
    }
    out.push_back(entry.as_string());
  }
  return out;
}

// Like `read_array`, but distinguishes "the key is absent" (nullopt) from "the
// key is present and explicitly empty" (an empty vector).
//
// The distinction matters wherever the field has a built-in default: a manifest
// that writes `sources = []` is disabling something, and silently substituting
// the default would invert the user's intent.
Result<std::optional<std::vector<std::string>>> read_array_optional(const Table& table,
                                                                   std::string_view key,
                                                                   std::string_view context) {
  if (!lookup(table, key)) {
    return std::optional<std::vector<std::string>>{};
  }
  auto values = read_array(table, key, context);
  if (!values) {
    return fail<std::optional<std::vector<std::string>>>(values.error());
  }
  return std::optional<std::vector<std::string>>{std::move(*values)};
}


std::optional<platform::Crt> crt_from_manifest(std::string_view text) {
  const std::string value = str::to_lower(str::trim_view(text));
  if (value == "glibc" || value == "gnu") {
    return platform::Crt::Glibc;
  }
  if (value == "musl") {
    return platform::Crt::Musl;
  }
  if (value == "uclibc") {
    return platform::Crt::Uclibc;
  }
  if (value == "msvc") {
    return platform::Crt::Msvc;
  }
  if (value == "wasi") {
    return platform::Crt::WasmWasi;
  }
  if (value == "none") {
    return platform::Crt::None;
  }
  return std::nullopt;
}

std::optional<platform::LibCpp> libcpp_from_manifest(std::string_view text) {
  const std::string value = str::to_lower(str::trim_view(text));
  if (value == "libstdc++" || value == "libstdcxx" || value == "stdc++") {
    return platform::LibCpp::LibStdCpp;
  }
  if (value == "libc++" || value == "libcxx") {
    return platform::LibCpp::LibCpp;
  }
  if (value == "msvc" || value == "msvc-stl") {
    return platform::LibCpp::MsvcStl;
  }
  if (value == "none" || value == "nostd") {
    return platform::LibCpp::NoStd;
  }
  return std::nullopt;
}

Result<Profile> parse_profile(const Table& table, const Profile& defaults,
                              std::string_view context) {
  Profile profile = defaults;
  if (auto value = lookup(table, "optimization")) {
    if (value && (*value)->is_integer()) {
      const std::int64_t level = (*value)->as_integer();
      if (level < 0 || level > 3) {
        return fail<Profile>(Error::make(
            "cova::manifest::profile",
            "optimization level " + std::to_string(level) + " is out of range in " +
                std::string(context))
                                 .with("accepted levels are 0, 1, 2 and 3")
                                 .with("use \"s\" or \"size\" to optimise for size"));
      }
      profile.optimization = static_cast<int>(level);
      profile.optimize_for_size = false;
    } else if (value && (*value)->is_string()) {
      const std::string text = (*value)->as_string();
      if (text == "none" || text == "0") {
        profile.optimization = 0;
        profile.optimize_for_size = false;
      } else if (text == "1" || text == "less") {
        profile.optimization = 1;
        profile.optimize_for_size = false;
      } else if (text == "2") {
        profile.optimization = 2;
        profile.optimize_for_size = false;
      } else if (text == "3") {
        profile.optimization = 3;
        profile.optimize_for_size = false;
      } else if (text == "s" || text == "size") {
        // `-Os` rather than an `-O` level: this is what the author asked for.
        profile.optimization = 2;
        profile.optimize_for_size = true;
      } else {
        return fail<Profile>(Error::make(
            "cova::manifest::profile", "invalid optimization `" + text + "` in " +
                                           std::string(context)));
      }
    } else if (value) {
      return fail<Profile>(Error::make("cova::manifest::type",
                                       "expected an integer or string for optimization in " +
                                           std::string(context)));
    }
  }
  auto debug = read_bool(table, "debug", context, profile.debug);
  if (!debug) {
    return fail<Profile>(debug.error());
  }
  profile.debug = *debug;
  auto lto = read_bool(table, "lto", context, profile.lto);
  if (!lto) {
    return fail<Profile>(lto.error());
  }
  profile.lto = *lto;
  auto assertions = read_bool(table, "assertions", context, profile.assertions);
  if (!assertions) {
    return fail<Profile>(assertions.error());
  }
  profile.assertions = *assertions;
  if (auto value = lookup(table, "sanitize")) {
    if ((*value)->is_bool()) {
      profile.sanitize = (*value)->as_bool();
    } else if ((*value)->is_string()) {
      profile.sanitize = true;
      profile.sanitizer = (*value)->as_string();
    }
  }
  if (auto value = lookup(table, "sanitizer")) {
    if ((*value)->is_string()) {
      profile.sanitize = true;
      profile.sanitizer = (*value)->as_string();
    }
  }
  auto cxxflags = read_array(table, "extra-cxxflags", context);
  if (!cxxflags) {
    return fail<Profile>(cxxflags.error());
  }
  profile.extra_cxxflags = std::move(*cxxflags);
  auto ldflags = read_array(table, "extra-ldflags", context);
  if (!ldflags) {
    return fail<Profile>(ldflags.error());
  }
  profile.extra_ldflags = std::move(*ldflags);
  return profile;
}

Result<ToolchainSection> parse_toolchain(const Table& table, std::string_view context) {
  ToolchainSection section;
  if (auto value = lookup(table, "policy")) {
    if (!(*value)->is_string()) {
      return fail<ToolchainSection>(
          Error::make("cova::manifest::type", "expected a string for toolchain.policy"));
    }
    auto policy = toolchain::policy_from_string((*value)->as_string());
    if (!policy) {
      return fail<ToolchainSection>(Error::make(
          "cova::manifest::toolchain",
          "unknown toolchain policy `" + (*value)->as_string() + "` in " + std::string(context)));
    }
    section.policy = *policy;
  }
  auto compiler = read_string(table, "compiler", context);
  if (!compiler) {
    return fail<ToolchainSection>(compiler.error());
  }
  section.compiler = *compiler;
  auto version = read_string(table, "version", context);
  if (!version) {
    return fail<ToolchainSection>(version.error());
  }
  section.version = *version;
  auto auto_install = read_bool(table, "auto-install", context, section.auto_install);
  if (!auto_install) {
    return fail<ToolchainSection>(auto_install.error());
  }
  section.auto_install = *auto_install;
  auto preferred = read_string(table, "preferred", context);
  if (!preferred) {
    return fail<ToolchainSection>(preferred.error());
  }
  section.preferred = *preferred;
  return section;
}

Result<FormatSection> parse_format(const Table& table, std::string_view context) {
  FormatSection section;
  auto tool = read_string(table, "tool", context, section.tool);
  if (!tool) {
    return fail<FormatSection>(tool.error());
  }
  section.tool = *tool;
  auto style = read_string(table, "style", context, section.style);
  if (!style) {
    return fail<FormatSection>(style.error());
  }
  section.style = *style;
  auto include = read_array(table, "include", context);
  if (!include) {
    return fail<FormatSection>(include.error());
  }
  section.include = std::move(*include);
  auto exclude = read_array(table, "exclude", context);
  if (!exclude) {
    return fail<FormatSection>(exclude.error());
  }
  section.exclude = std::move(*exclude);
  auto fallback = read_string(table, "fallback-style", context, section.fallback_style);
  if (!fallback) {
    return fail<FormatSection>(fallback.error());
  }
  section.fallback_style = *fallback;
  return section;
}

Result<TestSection> parse_test(const Table& table, std::string_view context) {
  TestSection section;
  // The default lives on the struct, so "absent" inherits it while an explicit
  // `sources = []` genuinely disables test discovery. Substituting the default
  // for an empty array made the key impossible to switch off.
  auto sources = read_array_optional(table, "sources", context);
  if (!sources) {
    return fail<TestSection>(sources.error());
  }
  if (sources->has_value()) {
    section.sources = std::move(**sources);
  }
  auto exclude = read_array(table, "exclude", context);
  if (!exclude) {
    return fail<TestSection>(exclude.error());
  }
  section.exclude = std::move(*exclude);
  auto runner = read_string(table, "runner", context, section.runner);
  if (!runner) {
    return fail<TestSection>(runner.error());
  }
  section.runner = *runner;
  auto runner_command = read_string(table, "runner-command", context, section.runner_command);
  if (!runner_command) {
    return fail<TestSection>(runner_command.error());
  }
  section.runner_command = *runner_command;
  return section;
}

Result<DependencySpec> parse_dependency(std::string name, const Value& value,
                                        const stdfs::path& root, bool dev,
                                        std::string_view context) {
  DependencySpec spec;
  spec.name = std::move(name);
  spec.dev = dev;
  if (value.is_string()) {
    spec.requirement_text = value.as_string();
  } else if (value.is_table()) {
    const Table& table = *value.as_table();
    auto version = read_string(table, "version", context);
    if (!version) {
      return fail<DependencySpec>(version.error());
    }
    spec.requirement_text = *version;
    if (auto path = lookup(table, "path")) {
      if (!(*path)->is_string()) {
        return fail<DependencySpec>(
            Error::make("cova::manifest::type", "dependency path must be a string"));
      }
      spec.path = fs::absolute_normalized(root / (*path)->as_string());
    }
    auto git = read_string(table, "git", context);
    if (!git) {
      return fail<DependencySpec>(git.error());
    }
    if (!git->empty()) {
      spec.git = *git;
    }
    auto rev = read_string(table, "rev", context);
    if (!rev) {
      return fail<DependencySpec>(rev.error());
    }
    if (rev->empty()) {
      auto branch = read_string(table, "branch", context);
      if (branch) {
        rev = branch;
      }
    }
    if (!rev->empty()) {
      spec.git_rev = *rev;
    }
    auto registry = read_string(table, "registry", context);
    if (!registry) {
      return fail<DependencySpec>(registry.error());
    }
    if (!registry->empty()) {
      spec.registry = *registry;
    }
    auto features = read_array(table, "features", context);
    if (!features) {
      return fail<DependencySpec>(features.error());
    }
    spec.features = std::move(*features);
    auto optional = read_bool(table, "optional", context, false);
    if (!optional) {
      return fail<DependencySpec>(optional.error());
    }
    spec.optional = *optional;
    auto dev_flag = read_bool(table, "dev", context, dev);
    if (!dev_flag) {
      return fail<DependencySpec>(dev_flag.error());
    }
    spec.dev = *dev_flag;
    auto targets = read_array(table, "target", context);
    if (!targets) {
      return fail<DependencySpec>(targets.error());
    }
    spec.target_filters = std::move(*targets);
  } else {
    return fail<DependencySpec>(
        Error::make("cova::manifest::type",
                    "dependency `" + spec.name + "` must be a version string or a table"));
  }

  if (!spec.requirement_text.empty()) {
    auto requirement = VersionRequirement::parse(spec.requirement_text);
    if (!requirement) {
      return fail<DependencySpec>(
          Error::make("cova::manifest::dependency_version",
                      "invalid version requirement `" + spec.requirement_text + "` for `" +
                          spec.name + "`")
              .with(requirement.error().message));
    }
    spec.requirements.push_back(*requirement);
  }
  return spec;
}

Result<TargetOverride> parse_target_override(const std::string& target_text, const Value& value,
                                             std::string_view context) {
  TargetOverride override;
  override.target = target_text;
  if (!value.is_table()) {
    return fail<TargetOverride>(
        Error::make("cova::manifest::type", "target override `" + target_text + "` must be a table"));
  }
  // Canonicalise the key when possible; a typo is reported by validate().
  if (auto parsed = platform::Triple::parse(target_text)) {
    override.target = parsed->to_string();
  }
  const Table& table = *value.as_table();
  if (auto kind = lookup(table, "type")) {
    if ((*kind)->is_string()) {
      if (auto parsed = kind_from_string((*kind)->as_string())) {
        override.kind = *parsed;
      }
    }
  }
  auto definitions = read_array(table, "definitions", context);
  if (!definitions) {
    return fail<TargetOverride>(definitions.error());
  }
  override.definitions = std::move(*definitions);
  auto compile_options = read_array(table, "compile-options", context);
  if (!compile_options) {
    return fail<TargetOverride>(compile_options.error());
  }
  override.compile_options = std::move(*compile_options);
  auto link_options = read_array(table, "link-options", context);
  if (!link_options) {
    return fail<TargetOverride>(link_options.error());
  }
  override.link_options = std::move(*link_options);
  auto deps = read_array(table, "dependencies", context);
  if (!deps) {
    return fail<TargetOverride>(deps.error());
  }
  override.dependencies = std::move(*deps);
  return override;
}

std::string requirement_text_for(const DependencySpec& spec) {
  if (!spec.requirement_text.empty()) {
    return spec.requirement_text;
  }
  if (!spec.requirements.empty()) {
    return spec.requirements.front().to_string();
  }
  return "*";
}

void write_string_array(Table& table, std::string_view key, const std::vector<std::string>& values) {
  if (values.empty()) {
    return;
  }
  toml::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  table.emplace(std::string(key), Value(std::move(array)));
}

Value table_value(Table table) { return Value(std::move(table)); }

}  // namespace

// ---------------------------------------------------------------------------
// Enum helpers and small models
// ---------------------------------------------------------------------------
std::string_view to_string(PackageKind kind) {
  switch (kind) {
    case PackageKind::Executable:
      return "executable";
    case PackageKind::StaticLibrary:
      return "static-library";
    case PackageKind::SharedLibrary:
      return "shared-library";
  }
  return "executable";
}

std::optional<PackageKind> kind_from_string(std::string_view text) {
  const std::string value = str::to_lower(str::trim_view(text));
  if (value == "executable" || value == "bin" || value == "binary" || value == "app") {
    return PackageKind::Executable;
  }
  if (value == "static-library" || value == "static_library" || value == "static" ||
      value == "lib" || value == "library") {
    return PackageKind::StaticLibrary;
  }
  if (value == "shared-library" || value == "shared_library" || value == "shared" ||
      value == "dylib" || value == "cdylib") {
    return PackageKind::SharedLibrary;
  }
  return std::nullopt;
}

int PackageSection::cpp_standard() const {
  std::string value = str::to_lower(str::trim_view(edition));
  // "gnu++" is five characters and "c++" is three; using 3 for both turned
  // "gnu++17" into "++17", which `stoi` rejects, and the old catch-all then
  // reported the newest standard (26) instead of the requested one.
  if (value.rfind("gnu++", 0) == 0) {
    value = value.substr(5);
  } else if (value.rfind("c++", 0) == 0) {
    value = value.substr(3);
  }
  // Require plain digits: `stoi` happily parses "17abc" as 17.
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(),
                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
    return 0;
  }
  int standard = 0;
  try {
    standard = std::stoi(value);
  } catch (...) {
    return 0;
  }
  // Only the standards Cova actually supports. Returning an arbitrary number
  // would let `c++99` through as if it were a real flag.
  switch (standard) {
    case 11:
    case 14:
    case 17:
    case 20:
    case 23:
    case 26:
      return standard;
    default:
      return 0;
  }
}

std::string DependencySpec::to_string() const {
  std::string out = name;
  if (!requirement_text.empty()) {
    out += " " + requirement_text;
  } else if (!requirements.empty()) {
    out += " " + requirements.front().to_string();
  }
  if (path) {
    out += " (path " + fs::generic_string(*path) + ")";
  } else if (git) {
    out += " (git ";
    out += *git;
    if (git_rev) {
      out += "#" + *git_rev;
    }
    out += ")";
  } else if (registry) {
    out += " (registry " + *registry + ")";
  }
  return out;
}

std::string Profile::to_string() const {
  const std::string level = optimize_for_size ? std::string("s") : std::to_string(optimization);
  return name + " (opt " + level + (debug ? ", debug" : "") +
         (lto ? ", lto" : "") + ")";
}

std::vector<std::string> Profile::cxx_flags() const {
  std::vector<std::string> flags;
  if (optimize_for_size) {
    flags.push_back("-Os");
  } else {
    flags.push_back("-O" + std::to_string(optimization));
  }
  if (debug) {
    flags.push_back("-g");
  }
  if (lto) {
    flags.push_back("-flto");
  }
  if (sanitize) {
    flags.push_back("-fsanitize=" + (sanitizer.empty() ? std::string("address") : sanitizer));
  }
  if (!assertions) {
    flags.push_back("-DNDEBUG");
  }
  flags.insert(flags.end(), extra_cxxflags.begin(), extra_cxxflags.end());
  return flags;
}

std::vector<std::string> Profile::ld_flags() const {
  std::vector<std::string> flags;
  if (lto) {
    flags.push_back("-flto");
  }
  if (sanitize) {
    flags.push_back("-fsanitize=" + (sanitizer.empty() ? std::string("address") : sanitizer));
  }
  flags.insert(flags.end(), extra_ldflags.begin(), extra_ldflags.end());
  return flags;
}

std::map<std::string, Profile, std::less<>> default_profiles() {
  std::map<std::string, Profile, std::less<>> profiles;
  Profile dev;
  dev.name = "dev";
  dev.optimization = 0;
  dev.debug = true;
  dev.assertions = true;
  profiles.emplace("dev", std::move(dev));

  Profile release;
  release.name = "release";
  release.optimization = 3;
  release.debug = false;
  release.lto = true;
  release.assertions = false;
  profiles.emplace("release", std::move(release));
  return profiles;
}

// ---------------------------------------------------------------------------
// Manifest accessors
// ---------------------------------------------------------------------------
const Profile& Manifest::profile(std::string_view name) const {
  auto it = profiles.find(name);
  if (it != profiles.end()) {
    return it->second;
  }
  static const Profile fallback = [] {
    Profile profile;
    profile.name = "dev";
    profile.optimization = 0;
    profile.debug = true;
    return profile;
  }();
  return fallback;
}

bool Manifest::is_workspace_root() const { return workspace.has_value(); }

std::vector<const DependencySpec*> Manifest::target_dependencies(
    const platform::Triple& target) const {
  std::vector<const DependencySpec*> out;
  const std::string canonical = target.to_string();
  const std::string arch = std::string(platform::to_string(target.arch));
  const std::string os = std::string(platform::to_string(target.os));
  for (const auto& dependency : dependencies) {
    if (dependency.target_filters.empty()) {
      out.push_back(&dependency);
      continue;
    }
    const bool matches = std::any_of(
        dependency.target_filters.begin(), dependency.target_filters.end(),
        [&](const std::string& filter) {
          const std::string value = str::to_lower(str::trim_view(filter));
          if (value == canonical || value == arch || value == os) {
            return true;
          }
          if (auto parsed = platform::Triple::parse(value)) {
            return *parsed == target;
          }
          return false;
        });
    if (matches) {
      out.push_back(&dependency);
    }
  }
  return out;
}

std::optional<platform::Triple> Manifest::declared_target() const {
  if (build.target.empty()) {
    return std::nullopt;
  }
  auto parsed = platform::Triple::parse(build.target);
  if (!parsed) {
    return std::nullopt;
  }
  return *parsed;
}

// ---------------------------------------------------------------------------
// Loading and parsing
// ---------------------------------------------------------------------------
Result<ManifestLoadResult> parse(std::string_view text, std::string_view source_name,
                                 const stdfs::path& root, const ManifestLoadOptions& options) {
  auto document = toml::parse(text, source_name);
  if (!document) {
    return fail<ManifestLoadResult>(document.error());
  }
  if (!document->is_table()) {
    return fail<ManifestLoadResult>(Error::make(
        "cova::manifest::structure", std::string(source_name) + " must contain a TOML table"));
  }
  const Table& top = *document->as_table();

  ManifestLoadResult result;
  Manifest& manifest = result.manifest;
  manifest.root = root;
  manifest.source_name = std::string(source_name);
  manifest.path = root.empty() ? stdfs::path(source_name) : root / "cova.toml";
  manifest.profiles = default_profiles();

  auto context = [&](std::string_view section) {
    return std::string(source_name) + " [" + std::string(section) + "]";
  };

  for (const auto& [key, value] : top) {
    if (key == "package") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[package]` must be a table in " +
                                                        std::string(source_name)));
      }
      const Table& table = *value.as_table();
      auto name = read_string(table, "name", context("package"));
      if (!name) {
        return fail<ManifestLoadResult>(name.error());
      }
      manifest.package.name = *name;
      auto version = read_string(table, "version", context("package"), "0.1.0");
      if (!version) {
        return fail<ManifestLoadResult>(version.error());
      }
      manifest.package.version = *version;
      auto edition = read_string(table, "edition", context("package"), "c++26");
      if (!edition) {
        return fail<ManifestLoadResult>(edition.error());
      }
      manifest.package.edition = *edition;
      auto description = read_string(table, "description", context("package"));
      if (!description) {
        return fail<ManifestLoadResult>(description.error());
      }
      manifest.package.description = *description;
      auto authors = read_array(table, "authors", context("package"));
      if (!authors) {
        return fail<ManifestLoadResult>(authors.error());
      }
      manifest.package.authors = std::move(*authors);
      auto license = read_string(table, "license", context("package"));
      if (!license) {
        return fail<ManifestLoadResult>(license.error());
      }
      manifest.package.license = *license;
      if (auto crt = lookup(table, "crt")) {
        if (!(*crt)->is_string() || !crt_from_manifest((*crt)->as_string())) {
          return fail<ManifestLoadResult>(Error::make(
              "cova::manifest::crt", "unknown C runtime `" + (*crt)->as_string() + "`"));
        }
        manifest.package.crt = crt_from_manifest((*crt)->as_string());
      }
      if (auto libcpp = lookup(table, "libcpp")) {
        if (!(*libcpp)->is_string() || !libcpp_from_manifest((*libcpp)->as_string())) {
          return fail<ManifestLoadResult>(Error::make(
              "cova::manifest::libcpp",
              "unknown C++ standard library `" + (*libcpp)->as_string() + "`"));
        }
        manifest.package.libcpp = libcpp_from_manifest((*libcpp)->as_string());
      }
    } else if (key == "build") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[build]` must be a table"));
      }
      const Table& table = *value.as_table();
      if (auto type = lookup(table, "type")) {
        if (!(*type)->is_string()) {
          return fail<ManifestLoadResult>(
              Error::make("cova::manifest::type", "`build.type` must be a string"));
        }
        auto kind = kind_from_string((*type)->as_string());
        if (!kind) {
          return fail<ManifestLoadResult>(Error::make(
              "cova::manifest::package_kind",
              "unknown package kind `" + (*type)->as_string() +
                  "`; expected executable, static-library or shared-library"));
        }
        manifest.build.kind = *kind;
      }
      auto sources = read_array(table, "sources", context("build"));
      if (!sources) {
        return fail<ManifestLoadResult>(sources.error());
      }
      manifest.build.sources = std::move(*sources);
      auto source_dirs = read_array(table, "source-dirs", context("build"));
      if (!source_dirs) {
        return fail<ManifestLoadResult>(source_dirs.error());
      }
      manifest.build.source_dirs = std::move(*source_dirs);
      auto public_includes = read_array(table, "public-includes", context("build"));
      if (!public_includes) {
        return fail<ManifestLoadResult>(public_includes.error());
      }
      manifest.build.public_includes = std::move(*public_includes);
      auto private_includes = read_array(table, "private-includes", context("build"));
      if (!private_includes) {
        return fail<ManifestLoadResult>(private_includes.error());
      }
      manifest.build.private_includes = std::move(*private_includes);
      auto public_definitions = read_array(table, "public-definitions", context("build"));
      if (!public_definitions) {
        return fail<ManifestLoadResult>(public_definitions.error());
      }
      manifest.build.public_definitions = std::move(*public_definitions);
      auto private_definitions = read_array(table, "private-definitions", context("build"));
      if (!private_definitions) {
        return fail<ManifestLoadResult>(private_definitions.error());
      }
      manifest.build.private_definitions = std::move(*private_definitions);
      auto compile_options = read_array(table, "compile-options", context("build"));
      if (!compile_options) {
        return fail<ManifestLoadResult>(compile_options.error());
      }
      manifest.build.compile_options = std::move(*compile_options);
      auto link_options = read_array(table, "link-options", context("build"));
      if (!link_options) {
        return fail<ManifestLoadResult>(link_options.error());
      }
      manifest.build.link_options = std::move(*link_options);
      auto link_libraries = read_array(table, "link-libraries", context("build"));
      if (!link_libraries) {
        return fail<ManifestLoadResult>(link_libraries.error());
      }
      manifest.build.link_libraries = std::move(*link_libraries);
      auto exclude = read_array(table, "exclude", context("build"));
      if (!exclude) {
        return fail<ManifestLoadResult>(exclude.error());
      }
      manifest.build.exclude = std::move(*exclude);
      auto entry_point = read_string(table, "entry-point", context("build"));
      if (!entry_point) {
        return fail<ManifestLoadResult>(entry_point.error());
      }
      manifest.build.entry_point = *entry_point;
      auto build_script = read_bool(table, "build-script", context("build"), false);
      if (!build_script) {
        return fail<ManifestLoadResult>(build_script.error());
      }
      manifest.build.build_script = *build_script;
      if (manifest.build.build_script) {
        return fail<ManifestLoadResult>(Error::make(
            "cova::manifest::build_script_unsupported",
            "`build-script = true` is not supported: Cova does not implement build scripts")
            .with("the declaration was rejected instead of being silently ignored")
            .with("remove it, or set `build-script = false`"));
      }
      auto target = read_string(table, "target", context("build"));
      if (!target) {
        return fail<ManifestLoadResult>(target.error());
      }
      manifest.build.target = *target;
      auto constraints = platform::constraints_from_table(&table, "manifest");
      if (!constraints) {
        return fail<ManifestLoadResult>(constraints.error());
      }
      manifest.constraints = std::move(*constraints);
    } else if (key == "dependencies" || key == "dev-dependencies") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[" + key + "]` must be a table"));
      }
      const bool dev = key == "dev-dependencies";
      for (const auto& [name, entry] : *value.as_table()) {
        auto dependency = parse_dependency(name, entry, root, dev, context(key));
        if (!dependency) {
          return fail<ManifestLoadResult>(dependency.error());
        }
        manifest.dependencies.push_back(std::move(*dependency));
      }
    } else if (key == "profile") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[profile]` must be a table"));
      }
      for (const auto& [name, entry] : *value.as_table()) {
        if (!entry.is_table()) {
          return fail<ManifestLoadResult>(Error::make(
              "cova::manifest::type", "`[profile." + name + "]` must be a table"));
        }
        auto base = manifest.profiles.find(name);
        Profile defaults = base != manifest.profiles.end() ? base->second : Profile{};
        defaults.name = name;
        auto profile = parse_profile(*entry.as_table(), defaults, context("profile." + name));
        if (!profile) {
          return fail<ManifestLoadResult>(profile.error());
        }
        manifest.profiles[name] = std::move(*profile);
      }
    } else if (key == "toolchain") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[toolchain]` must be a table"));
      }
      auto section = parse_toolchain(*value.as_table(), context("toolchain"));
      if (!section) {
        return fail<ManifestLoadResult>(section.error());
      }
      manifest.toolchain = std::move(*section);
    } else if (key == "format") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[format]` must be a table"));
      }
      auto section = parse_format(*value.as_table(), context("format"));
      if (!section) {
        return fail<ManifestLoadResult>(section.error());
      }
      manifest.format = std::move(*section);
    } else if (key == "test") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[test]` must be a table"));
      }
      auto section = parse_test(*value.as_table(), context("test"));
      if (!section) {
        return fail<ManifestLoadResult>(section.error());
      }
      manifest.test = std::move(*section);
    } else if (key == "workspace") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[workspace]` must be a table"));
      }
      WorkspaceSection section;
      const Table& table = *value.as_table();
      auto members = read_array(table, "members", context("workspace"));
      if (!members) {
        return fail<ManifestLoadResult>(members.error());
      }
      section.members = std::move(*members);
      auto exclude = read_array(table, "exclude", context("workspace"));
      if (!exclude) {
        return fail<ManifestLoadResult>(exclude.error());
      }
      section.exclude = std::move(*exclude);
      if (auto toolchain_value = lookup(table, "toolchain")) {
        if ((*toolchain_value)->is_table()) {
          auto policy = parse_toolchain(*(*toolchain_value)->as_table(),
                                        context("workspace.toolchain"));
          if (!policy) {
            return fail<ManifestLoadResult>(policy.error());
          }
          section.toolchain = std::move(*policy);
        }
      }
      if (auto format_value = lookup(table, "format")) {
        if ((*format_value)->is_table()) {
          auto format = parse_format(*(*format_value)->as_table(), context("workspace.format"));
          if (!format) {
            return fail<ManifestLoadResult>(format.error());
          }
          section.format = std::move(*format);
        }
      }
      manifest.workspace = std::move(section);
    } else if (key == "targets") {
      if (!value.is_table()) {
        return fail<ManifestLoadResult>(Error::make("cova::manifest::type",
                                                    "`[targets]` must be a table"));
      }
      for (const auto& [target_name, entry] : *value.as_table()) {
        auto override_value = parse_target_override(target_name, entry, context("targets"));
        if (!override_value) {
          return fail<ManifestLoadResult>(override_value.error());
        }
        manifest.target_overrides[override_value->target] = std::move(*override_value);
      }
    } else if (options.strict) {
      result.warnings.push_back("unknown top-level key `" + key + "` in " + std::string(source_name));
    }
  }

  if (options.apply_defaults) {
    if (manifest.build.sources.empty() && manifest.build.source_dirs.empty() &&
        !manifest.is_workspace_root()) {
      manifest.build.sources = {"src/**/*.cpp"};
    }
    if (manifest.build.public_includes.empty() &&
        manifest.build.kind == PackageKind::StaticLibrary && !manifest.is_workspace_root()) {
      manifest.build.public_includes = {"include"};
    }
  }

  return result;
}

Result<ManifestLoadResult> load(const stdfs::path& manifest_path,
                                const ManifestLoadOptions& options) {
  auto contents = fs::read_file(manifest_path);
  if (!contents) {
    Error error = Error::make("cova::manifest::read",
                              "could not read " + fs::generic_string(manifest_path));
    error.context = contents.error().context;
    error.with(contents.error().message);
    return fail<ManifestLoadResult>(std::move(error));
  }
  const auto root = manifest_path.has_parent_path() ? manifest_path.parent_path()
                                                    : stdfs::path(".");
  return parse(*contents, fs::generic_string(manifest_path), root, options);
}

Result<ManifestLoadResult> load_from_root(const stdfs::path& root,
                                          const ManifestLoadOptions& options) {
  return load(root / "cova.toml", options);
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
std::string to_toml(const Manifest& manifest) {
  Table top;

  if (!manifest.package.name.empty()) {
    Table package;
    package.emplace("name", Value(manifest.package.name));
    package.emplace("version", Value(manifest.package.version));
    package.emplace("edition", Value(manifest.package.edition));
    if (!manifest.package.description.empty()) {
      package.emplace("description", Value(manifest.package.description));
    }
    write_string_array(package, "authors", manifest.package.authors);
    if (!manifest.package.license.empty()) {
      package.emplace("license", Value(manifest.package.license));
    }
    if (manifest.package.crt) {
      package.emplace("crt", Value(std::string(platform::to_string(*manifest.package.crt))));
    }
    if (manifest.package.libcpp) {
      package.emplace("libcpp",
                      Value(std::string(platform::to_string(*manifest.package.libcpp))));
    }
    top.emplace("package", table_value(std::move(package)));
  }

  {
    Table build;
    build.emplace("type", Value(std::string(to_string(manifest.build.kind))));
    write_string_array(build, "sources", manifest.build.sources);
    write_string_array(build, "source-dirs", manifest.build.source_dirs);
    write_string_array(build, "public-includes", manifest.build.public_includes);
    write_string_array(build, "private-includes", manifest.build.private_includes);
    write_string_array(build, "public-definitions", manifest.build.public_definitions);
    write_string_array(build, "private-definitions", manifest.build.private_definitions);
    write_string_array(build, "compile-options", manifest.build.compile_options);
    write_string_array(build, "link-options", manifest.build.link_options);
    write_string_array(build, "link-libraries", manifest.build.link_libraries);
    write_string_array(build, "exclude", manifest.build.exclude);
    if (!manifest.build.entry_point.empty()) {
      build.emplace("entry-point", Value(manifest.build.entry_point));
    }
    if (manifest.build.build_script) {
      build.emplace("build-script", Value(true));
    }
    if (!manifest.build.target.empty()) {
      build.emplace("target", Value(manifest.build.target));
    }
    // Preserve extra [build] constraints.
    for (const auto& constraint : manifest.constraints) {
      switch (constraint.kind) {
        case platform::ConstraintKind::Arch:
          build.emplace("cpu-arch", Value(constraint.value));
          break;
        case platform::ConstraintKind::CpuMicroArch:
          build.emplace("cpu", Value(constraint.value));
          break;
        case platform::ConstraintKind::CpuFeature: {
          toml::Array features;
          for (const auto& other : manifest.constraints) {
            if (other.kind == platform::ConstraintKind::CpuFeature) {
              features.emplace_back(other.value);
            }
          }
          build["features"] = Value(std::move(features));
          break;
        }
        case platform::ConstraintKind::LinkMode:
          build.emplace("link", Value(constraint.value));
          break;
        case platform::ConstraintKind::Exceptions:
          build.emplace("exceptions", Value(constraint.value));
          break;
        case platform::ConstraintKind::Rtti:
          build.emplace("rtti", Value(constraint.value));
          break;
        case platform::ConstraintKind::ThreadModel:
          build.emplace("threads", Value(constraint.value));
          break;
        case platform::ConstraintKind::Crt:
          build.emplace("crt", Value(constraint.value));
          break;
        case platform::ConstraintKind::LibCpp:
          build.emplace("libcpp", Value(constraint.value));
          break;
        case platform::ConstraintKind::OsVersion:
          build.emplace("os-version", Value(constraint.value));
          break;
        case platform::ConstraintKind::SdkVersion:
          build.emplace("sdk", Value(constraint.value));
          break;
        case platform::ConstraintKind::Abi:
          build.emplace("abi", Value(constraint.value));
          break;
        case platform::ConstraintKind::Unknown:
          break;
      }
    }
    top.emplace("build", table_value(std::move(build)));
  }

  if (!manifest.dependencies.empty()) {
    Table dependencies;
    Table dev_dependencies;
    for (const auto& dependency : manifest.dependencies) {
      const std::string version = requirement_text_for(dependency);
      Value entry;
      const bool simple = dependency.path.has_value() == false &&
                          dependency.git.has_value() == false &&
                          dependency.registry.has_value() == false &&
                          dependency.features.empty() && !dependency.optional &&
                          dependency.target_filters.empty();
      if (simple) {
        entry = Value(version);
      } else {
        Table table;
        if (!version.empty() && version != "*") {
          table.emplace("version", Value(version));
        }
        if (dependency.path) {
          std::error_code ec;
          auto relative = stdfs::relative(*dependency.path, manifest.root, ec);
          table.emplace("path", Value(ec ? fs::generic_string(*dependency.path)
                                         : fs::generic_string(relative)));
        }
        if (dependency.git) {
          table.emplace("git", Value(*dependency.git));
        }
        if (dependency.git_rev) {
          table.emplace("rev", Value(*dependency.git_rev));
        }
        if (dependency.registry) {
          table.emplace("registry", Value(*dependency.registry));
        }
        write_string_array(table, "features", dependency.features);
        if (dependency.optional) {
          table.emplace("optional", Value(true));
        }
        write_string_array(table, "target", dependency.target_filters);
        entry = table_value(std::move(table));
      }
      if (dependency.dev) {
        dev_dependencies.emplace(dependency.name, std::move(entry));
      } else {
        dependencies.emplace(dependency.name, std::move(entry));
      }
    }
    if (!dependencies.empty()) {
      top.emplace("dependencies", table_value(std::move(dependencies)));
    }
    if (!dev_dependencies.empty()) {
      top.emplace("dev-dependencies", table_value(std::move(dev_dependencies)));
    }
  }

  if (!manifest.profiles.empty()) {
    Table profiles;
    for (const auto& [name, profile] : manifest.profiles) {
      Table table;
      // Preserve `optimization = "s"` as such instead of flattening it to the
      // numeric level 2, which would re-parse as `-O2`.
      table.emplace("optimization", profile.optimize_for_size
                                        ? Value(std::string("s"))
                                        : Value(static_cast<std::int64_t>(profile.optimization)));
      // `debug`, `lto` and `assertions` are always written, even when they
      // match the built-in default for this profile name. Parsing starts from
      // `default_profiles()`, so an omitted key re-applies the *default*, not
      // the value that was in the file: `lto = false` in `[profile.release]`
      // silently became `lto = true` on the next write.
      table.emplace("debug", Value(profile.debug));
      table.emplace("lto", Value(profile.lto));
      table.emplace("assertions", Value(profile.assertions));
      if (profile.sanitize) {
        if (profile.sanitizer.empty()) {
          table.emplace("sanitize", Value(true));
        } else {
          table.emplace("sanitizer", Value(profile.sanitizer));
        }
      }
      write_string_array(table, "extra-cxxflags", profile.extra_cxxflags);
      write_string_array(table, "extra-ldflags", profile.extra_ldflags);
      profiles.emplace(name, table_value(std::move(table)));
    }
    top.emplace("profile", table_value(std::move(profiles)));
  }

  {
    Table toolchain_section;
    toolchain_section.emplace("policy",
                              Value(std::string(toolchain::to_string(manifest.toolchain.policy))));
    if (!manifest.toolchain.compiler.empty()) {
      toolchain_section.emplace("compiler", Value(manifest.toolchain.compiler));
    }
    if (!manifest.toolchain.version.empty()) {
      toolchain_section.emplace("version", Value(manifest.toolchain.version));
    }
    toolchain_section.emplace("auto-install", Value(manifest.toolchain.auto_install));
    if (!manifest.toolchain.preferred.empty()) {
      toolchain_section.emplace("preferred", Value(manifest.toolchain.preferred));
    }
    top.emplace("toolchain", table_value(std::move(toolchain_section)));
  }

  {
    Table format;
    format.emplace("tool", Value(manifest.format.tool));
    format.emplace("style", Value(manifest.format.style));
    write_string_array(format, "include", manifest.format.include);
    write_string_array(format, "exclude", manifest.format.exclude);
    format.emplace("fallback-style", Value(manifest.format.fallback_style));
    top.emplace("format", table_value(std::move(format)));
  }

  {
    Table test;
    // `sources` is written even when empty: an empty array is how a manifest
    // disables test discovery, and omitting the key would re-apply the default
    // on the next parse (breaking round-trip stability and silently
    // re-enabling tests the user turned off).
    toml::Array sources;
    sources.reserve(manifest.test.sources.size());
    for (const auto& entry : manifest.test.sources) {
      sources.emplace_back(entry);
    }
    test.emplace("sources", Value(std::move(sources)));
    write_string_array(test, "exclude", manifest.test.exclude);
    if (!manifest.test.runner.empty()) {
      test.emplace("runner", Value(manifest.test.runner));
    }
    if (!manifest.test.runner_command.empty()) {
      test.emplace("runner-command", Value(manifest.test.runner_command));
    }
    top.emplace("test", table_value(std::move(test)));
  }

  if (manifest.workspace) {
    Table workspace;
    write_string_array(workspace, "members", manifest.workspace->members);
    write_string_array(workspace, "exclude", manifest.workspace->exclude);
    if (manifest.workspace->toolchain) {
      Table toolchain_section;
      toolchain_section.emplace(
          "policy",
          Value(std::string(toolchain::to_string(manifest.workspace->toolchain->policy))));
      if (!manifest.workspace->toolchain->compiler.empty()) {
        toolchain_section.emplace("compiler", Value(manifest.workspace->toolchain->compiler));
      }
      if (!manifest.workspace->toolchain->version.empty()) {
        toolchain_section.emplace("version", Value(manifest.workspace->toolchain->version));
      }
      // Mirror the top-level [toolchain] writer. Omitting `auto-install` and
      // `preferred` here silently dropped a workspace-level toolchain pin on
      // every manifest rewrite, falling back to system discovery.
      toolchain_section.emplace("auto-install", Value(manifest.workspace->toolchain->auto_install));
      if (!manifest.workspace->toolchain->preferred.empty()) {
        toolchain_section.emplace("preferred", Value(manifest.workspace->toolchain->preferred));
      }
      workspace.emplace("toolchain", table_value(std::move(toolchain_section)));
    }
    if (manifest.workspace->format) {
      Table format;
      format.emplace("tool", Value(manifest.workspace->format->tool));
      format.emplace("style", Value(manifest.workspace->format->style));
      format.emplace("fallback-style", Value(manifest.workspace->format->fallback_style));
      write_string_array(format, "include", manifest.workspace->format->include);
      write_string_array(format, "exclude", manifest.workspace->format->exclude);
      workspace.emplace("format", table_value(std::move(format)));
    }
    top.emplace("workspace", table_value(std::move(workspace)));
  }

  if (!manifest.target_overrides.empty()) {
    Table targets;
    for (const auto& [name, override] : manifest.target_overrides) {
      Table table;
      if (override.kind) {
        table.emplace("type", Value(std::string(to_string(*override.kind))));
      }
      write_string_array(table, "definitions", override.definitions);
      write_string_array(table, "compile-options", override.compile_options);
      write_string_array(table, "link-options", override.link_options);
      write_string_array(table, "dependencies", override.dependencies);
      targets.emplace(name, table_value(std::move(table)));
    }
    top.emplace("targets", table_value(std::move(targets)));
  }

  return toml::write(top);
}

// ---------------------------------------------------------------------------
// Defaults and validation
// ---------------------------------------------------------------------------
Manifest default_manifest(std::string_view name, PackageKind kind) {
  Manifest manifest;
  manifest.package.name = std::string(name);
  manifest.package.version = "0.1.0";
  manifest.package.edition = "c++26";
  manifest.build.kind = kind;
  manifest.build.sources = {"src/**/*.cpp"};
  if (kind == PackageKind::StaticLibrary || kind == PackageKind::SharedLibrary) {
    manifest.build.public_includes = {"include"};
  }
  manifest.profiles = default_profiles();
  manifest.toolchain.policy = toolchain::Policy::SystemFirst;
  manifest.toolchain.auto_install = true;
  manifest.test.sources = {"tests/**/*.cpp"};
  manifest.format.tool = "clang-format";
  manifest.format.style = "file";
  return manifest;
}

Manifest default_workspace_manifest(std::string_view name) {
  Manifest manifest;
  manifest.package.name.clear();
  manifest.workspace = WorkspaceSection{};
  manifest.workspace->members = {"packages/*"};
  manifest.profiles = default_profiles();
  manifest.toolchain.policy = toolchain::Policy::SystemFirst;
  manifest.toolchain.auto_install = true;
  manifest.format.tool = "clang-format";
  manifest.format.style = "file";
  manifest.test.sources = {"tests/**/*.cpp"};
  (void)name;
  return manifest;
}

Result<std::vector<std::string>> validate(const Manifest& manifest) {
  std::vector<std::string> warnings;
  if (!manifest.is_workspace_root() && manifest.package.name.empty()) {
    return fail<std::vector<std::string>>(
        Error::make("cova::manifest::missing_name", "manifest is missing `package.name`"));
  }
  if (!manifest.package.name.empty() && !str::is_package_name(manifest.package.name)) {
    return fail<std::vector<std::string>>(Error::make(
        "cova::manifest::invalid_name",
        "`" + manifest.package.name +
            "` is not a valid package name (lowercase letters, digits, '-' and '_' only)"));
  }
  auto version = Version::parse(manifest.package.version);
  if (!version) {
    return fail<std::vector<std::string>>(
        Error::make("cova::manifest::version", "invalid package version `" +
                                                   manifest.package.version + "`")
            .with(version.error().message));
  }
  const std::string edition = str::to_lower(manifest.package.edition);
  static const std::vector<std::string> editions = {"c++11", "c++14", "c++17", "c++20",
                                                    "c++23", "c++26", "gnu++11", "gnu++14",
                                                    "gnu++17", "gnu++20", "gnu++23", "gnu++26"};
  if (std::find(editions.begin(), editions.end(), edition) == editions.end()) {
    return fail<std::vector<std::string>>(Error::make(
        "cova::manifest::edition", "unsupported C++ edition `" + manifest.package.edition + "`"));
  }
  if (!manifest.is_workspace_root()) {
    if (manifest.build.sources.empty() && manifest.build.source_dirs.empty() &&
        manifest.build.entry_point.empty()) {
      warnings.push_back("`[build] sources` is empty; the package has nothing to compile");
    }
    if (manifest.build.kind == PackageKind::Executable && !manifest.build.entry_point.empty()) {
      // An explicit entry point is allowed to live outside the source glob.
    }
  }
  if (!manifest.is_workspace_root() && manifest.build.build_script) {
    return fail<std::vector<std::string>>(Error::make(
        "cova::manifest::build_script_unsupported",
        "`build-script = true` is not supported: Cova does not implement build scripts")
        .with("the declaration was rejected instead of being silently ignored"));
  }
  if (!manifest.build.target.empty()) {
    auto parsed = platform::Triple::parse(manifest.build.target);
    if (!parsed) {
      return fail<std::vector<std::string>>(
          Error::make("cova::manifest::target",
                      "invalid `[build] target` `" + manifest.build.target + "`")
              .with(parsed.error().message));
    }
  }
  std::set<std::string> seen;
  for (const auto& dependency : manifest.dependencies) {
    if (dependency.name.empty()) {
      return fail<std::vector<std::string>>(
          Error::make("cova::manifest::dependency", "a dependency has an empty name"));
    }
    if (!seen.insert(dependency.name).second) {
      return fail<std::vector<std::string>>(Error::make(
          "cova::manifest::dependency_duplicate", "duplicate dependency `" + dependency.name + "`"));
    }
    if (dependency.path && !dependency.git && !dependency.registry && !dependency.requirement_text.empty()) {
      auto requirement = VersionRequirement::parse(dependency.requirement_text);
      if (!requirement) {
        return fail<std::vector<std::string>>(
            Error::make("cova::manifest::dependency_version",
                        "invalid version requirement for path dependency `" + dependency.name + "`"));
      }
    }
  }
  if (manifest.workspace) {
    if (manifest.workspace->members.empty()) {
      warnings.push_back("`[workspace] members` is empty");
    }
  }
  return warnings;
}

Result<stdfs::path> find_manifest_upwards(const stdfs::path& start) {
  auto current = fs::absolute_normalized(start);
  if (!fs::is_directory(current)) {
    current = current.parent_path();
  }
  while (!current.empty()) {
    const auto candidate = current / "cova.toml";
    if (fs::is_regular_file(candidate)) {
      return candidate;
    }
    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  return fail<stdfs::path>(Error::make(
      "cova::manifest::not_found",
      "could not find `cova.toml` at or above " + fs::generic_string(start)));
}

}  // namespace cova::manifest
