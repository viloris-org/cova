// Cova — layered configuration (implementation).
//
// Precedence is documented in config.hpp: CLI > project > workspace > user >
// defaults. `merge` here applies the user layer and the CLI layer; project and
// workspace layers are folded in by the caller (cli/workspace) before calling,
// because only the CLI knows which manifest it loaded.
#include "core/config/config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "core/diagnostics/diagnostic.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/toml/toml.hpp"

namespace cova::config {
namespace {

bool parse_bool_text(std::string_view text, bool& out) {
  if (str::iequals(text, "true") || text == "1" || str::iequals(text, "yes") ||
      str::iequals(text, "on")) {
    out = true;
    return true;
  }
  if (str::iequals(text, "false") || text == "0" || str::iequals(text, "no") ||
      str::iequals(text, "off")) {
    out = false;
    return true;
  }
  return false;
}

bool parse_int_text(std::string_view text, std::int64_t& out) {
  const std::string trimmed = str::trim(text);
  if (trimmed.empty()) {
    return false;
  }
  const auto result =
      std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), out);
  return result.ec == std::errc() && result.ptr == trimmed.data() + trimmed.size();
}

Error type_error(std::string_view code, std::string key, std::string_view expected,
                 const toml::Value& found) {
  Error error = Error::make(std::string(code), "invalid value for '" + std::string(key) +
                                                   "': expected " + std::string(expected) +
                                                   ", found " + found.type_name());
  if (found.location().line != 0) {
    error.with("line: " + std::to_string(found.location().line));
    error.with("column: " + std::to_string(found.location().column));
  }
  return error;
}

Result<std::uint64_t> read_size(const toml::Table& table, std::string_view key,
                                std::uint64_t fallback) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return fallback;
  }
  if (it->second.is_integer()) {
    const std::int64_t value = it->second.as_integer();
    if (value < 0) {
      return std::unexpected(Error::make("cova::config::value",
                                         "size for '" + std::string(key) +
                                             "' must not be negative"));
    }
    return static_cast<std::uint64_t>(value);
  }
  if (it->second.is_string()) {
    // Human-friendly sizes such as "512MiB" are accepted for convenience.
    const std::string text = str::to_lower(str::trim(it->second.as_string()));
    std::uint64_t multiplier = 1;
    std::string number = text;
    for (const auto& [suffix, factor] :
         std::vector<std::pair<std::string_view, std::uint64_t>>{{"kib", 1024ull},
                                                                 {"mib", 1024ull * 1024},
                                                                 {"gib", 1024ull * 1024 * 1024},
                                                                 {"kb", 1000ull},
                                                                 {"mb", 1000ull * 1000},
                                                                 {"gb", 1000ull * 1000 * 1000},
                                                                 {"b", 1ull}}) {
      if (str::ends_with(text, suffix)) {
        multiplier = factor;
        number = str::trim(text.substr(0, text.size() - suffix.size()));
        break;
      }
    }
    std::int64_t amount = 0;
    if (!parse_int_text(number, amount) || amount < 0) {
      return std::unexpected(Error::make("cova::config::value",
                                         "cannot parse size for '" + std::string(key) + "'")
                                 .with("value: " + it->second.as_string()));
    }
    // Reject a size that does not fit in 64 bits instead of wrapping. A wrapped
    // limit such as "9223372036854775807gb" -> 18446744072709551616 disables
    // cache eviction in a way the user never asked for.
    const auto scaled =
        static_cast<std::uint64_t>(amount) * multiplier;
    if (multiplier != 0 && scaled / multiplier != static_cast<std::uint64_t>(amount)) {
      return std::unexpected(Error::make("cova::config::value",
                                         "size for '" + std::string(key) + "' is too large")
                                 .with("value: " + it->second.as_string()));
    }
    return scaled;
  }
  return std::unexpected(type_error("cova::config::value", std::string(key), "an integer", it->second));
}

Result<bool> read_bool(const toml::Table& table, std::string_view key, bool fallback) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return fallback;
  }
  if (!it->second.is_bool()) {
    return std::unexpected(
        type_error("cova::config::value", std::string(key), "a boolean", it->second));
  }
  return it->second.as_bool();
}

Result<std::string> read_string(const toml::Table& table, std::string_view key,
                                std::string fallback) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return fallback;
  }
  if (!it->second.is_string()) {
    return std::unexpected(
        type_error("cova::config::value", std::string(key), "a string", it->second));
  }
  return it->second.as_string();
}

Result<std::int64_t> read_integer(const toml::Table& table, std::string_view key,
                                 std::int64_t fallback, std::int64_t minimum,
                                 std::int64_t maximum) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return fallback;
  }
  if (!it->second.is_integer()) {
    return std::unexpected(
        type_error("cova::config::value", std::string(key), "an integer", it->second));
  }
  const std::int64_t value = it->second.as_integer();
  if (value < minimum || value > maximum) {
    Error error = Error::make("cova::config::value", "value for '" + std::string(key) +
                                                         "' is out of range");
    error.with("value: " + std::to_string(value));
    error.with("expected: " + std::to_string(minimum) + ".." + std::to_string(maximum));
    return std::unexpected(std::move(error));
  }
  return value;
}

const toml::Table* sub_table(const toml::Table& table, std::string_view key) {
  const auto it = table.find(key);
  if (it == table.end()) {
    return nullptr;
  }
  return it->second.as_table();
}

// Keys `load_user_config` consumes; anything else is preserved in `extras`.
bool is_known_section(std::string_view key) {
  return key == "install" || key == "cache" || key == "network" || key == "runtime" ||
         key == "data_dir";
}

}  // namespace

// ---------------------------------------------------------------------------
// UserConfig
// ---------------------------------------------------------------------------

std::string UserConfig::to_toml() const {
  toml::Table root;

  if (data_dir_override) {
    root.emplace("data_dir", toml::Value(fs::generic_string(*data_dir_override)));
  }

  {
    toml::Table section;
    section.emplace("confirm_threshold_bytes",
                    toml::Value(static_cast<std::int64_t>(install.confirm_threshold_bytes)));
    section.emplace("allow_install", toml::Value(install.allow_install));
    root.emplace("install", toml::Value(std::move(section)));
  }
  {
    toml::Table section;
    section.emplace("enabled", toml::Value(cache.enabled));
    section.emplace("max_size_bytes",
                    toml::Value(static_cast<std::int64_t>(cache.max_size_bytes)));
    section.emplace("read_only", toml::Value(cache.read_only));
    root.emplace("cache", toml::Value(std::move(section)));
  }
  {
    toml::Table section;
    section.emplace("timeout_ms", toml::Value(static_cast<std::int64_t>(network.timeout_ms)));
    section.emplace("retries", toml::Value(static_cast<std::int64_t>(network.retries)));
    toml::Array registries;
    for (const std::string& registry : network.trusted_registries) {
      registries.emplace_back(registry);
    }
    section.emplace("trusted_registries", toml::Value(std::move(registries)));
    root.emplace("network", toml::Value(std::move(section)));
  }
  {
    toml::Table section;
    section.emplace("default_runner", toml::Value(runtime.default_runner));
    section.emplace("qemu_prefix", toml::Value(runtime.qemu_prefix));
    section.emplace("prefer_native", toml::Value(runtime.prefer_native));
    root.emplace("runtime", toml::Value(std::move(section)));
  }

  // Forward-compatible extras, appended deterministically.
  for (const auto& [key, value] : extras) {
    if (root.find(key) == root.end()) {
      root.emplace(key, value);
    }
  }

  std::string out;
  out += "# Cova user configuration.\n";
  out += "# Precedence: CLI > project cova.toml > workspace cova.toml > this file > defaults.\n\n";
  out += toml::write(root);
  return out;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

Result<UserConfig> parse_user_config(std::string_view text, std::string_view source_name,
                                     const stdfs::path& path) {
  auto parsed = toml::parse(text, source_name);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  const toml::Table* root = parsed->as_table();
  if (root == nullptr) {
    return std::unexpected(Error::make("cova::config::parse",
                                       "configuration root must be a table")
                               .with("source: " + std::string(source_name)));
  }

  UserConfig config;
  config.path = path;

  if (const auto it = root->find("data_dir"); it != root->end()) {
    if (!it->second.is_string()) {
      return std::unexpected(
          type_error("cova::config::value", "data_dir", "a string", it->second));
    }
    config.data_dir_override = stdfs::path(it->second.as_string());
  }

  if (const toml::Table* install = sub_table(*root, "install"); install != nullptr) {
    auto threshold = read_size(*install, "confirm_threshold_bytes",
                               config.install.confirm_threshold_bytes);
    if (!threshold) {
      return std::unexpected(std::move(threshold.error()));
    }
    config.install.confirm_threshold_bytes = *threshold;
    auto allow = read_bool(*install, "allow_install", config.install.allow_install);
    if (!allow) {
      return std::unexpected(std::move(allow.error()));
    }
    config.install.allow_install = *allow;
  }

  if (const toml::Table* cache = sub_table(*root, "cache"); cache != nullptr) {
    auto enabled = read_bool(*cache, "enabled", config.cache.enabled);
    if (!enabled) {
      return std::unexpected(std::move(enabled.error()));
    }
    config.cache.enabled = *enabled;
    auto max_size = read_size(*cache, "max_size_bytes", config.cache.max_size_bytes);
    if (!max_size) {
      return std::unexpected(std::move(max_size.error()));
    }
    config.cache.max_size_bytes = *max_size;
    auto read_only = read_bool(*cache, "read_only", config.cache.read_only);
    if (!read_only) {
      return std::unexpected(std::move(read_only.error()));
    }
    config.cache.read_only = *read_only;
  }

  if (const toml::Table* network = sub_table(*root, "network"); network != nullptr) {
    auto timeout = read_integer(*network, "timeout_ms", config.network.timeout_ms, 0, 86'400'000);
    if (!timeout) {
      return std::unexpected(std::move(timeout.error()));
    }
    config.network.timeout_ms = static_cast<int>(*timeout);
    auto retries = read_integer(*network, "retries", config.network.retries, 0, 1000);
    if (!retries) {
      return std::unexpected(std::move(retries.error()));
    }
    config.network.retries = static_cast<int>(*retries);
    if (const auto it = network->find("trusted_registries"); it != network->end()) {
      if (!it->second.is_array()) {
        return std::unexpected(type_error("cova::config::value", "network.trusted_registries",
                                          "an array of strings", it->second));
      }
      for (const toml::Value& entry : *it->second.as_array()) {
        if (!entry.is_string()) {
          return std::unexpected(type_error("cova::config::value",
                                            "network.trusted_registries",
                                            "an array of strings", entry));
        }
        config.network.trusted_registries.push_back(entry.as_string());
      }
      // Deterministic order for serialisation.
      std::sort(config.network.trusted_registries.begin(),
                config.network.trusted_registries.end());
    }
  }

  if (const toml::Table* runtime = sub_table(*root, "runtime"); runtime != nullptr) {
    auto runner = read_string(*runtime, "default_runner", config.runtime.default_runner);
    if (!runner) {
      return std::unexpected(std::move(runner.error()));
    }
    config.runtime.default_runner = *runner;
    auto prefix = read_string(*runtime, "qemu_prefix", config.runtime.qemu_prefix);
    if (!prefix) {
      return std::unexpected(std::move(prefix.error()));
    }
    config.runtime.qemu_prefix = *prefix;
    auto prefer = read_bool(*runtime, "prefer_native", config.runtime.prefer_native);
    if (!prefer) {
      return std::unexpected(std::move(prefer.error()));
    }
    config.runtime.prefer_native = *prefer;
  }

  for (const auto& [key, value] : *root) {
    if (!is_known_section(key)) {
      config.extras.emplace(key, value);
    }
  }
  return config;
}

Result<UserConfig> load_user_config(const LoadOptions& options) {
  stdfs::path path;
  if (options.config_path) {
    path = *options.config_path;
  } else {
    auto dirs = fs::cova_dirs();
    if (!dirs) {
      return std::unexpected(std::move(dirs.error()));
    }
    path = dirs->config_file;
  }

  if (!fs::is_regular_file(path)) {
    if (options.require_file) {
      return std::unexpected(Error::make("cova::config::missing", "configuration file not found")
                                 .with("path: " + fs::generic_string(path)));
    }
    UserConfig defaults;
    defaults.path = path;
    return defaults;
  }

  auto contents = fs::read_file(path);
  if (!contents) {
    return std::unexpected(std::move(contents.error()));
  }
  return parse_user_config(*contents, fs::generic_string(path), path);
}

Status save_user_config(const UserConfig& config) {
  stdfs::path path = config.path;
  if (path.empty()) {
    auto dirs = fs::cova_dirs();
    if (!dirs) {
      return std::unexpected(std::move(dirs.error()));
    }
    path = dirs->config_file;
  }
  // Atomic write: an interrupted save must not truncate the user's config.
  return fs::write_file_atomic(path, config.to_toml());
}

// ---------------------------------------------------------------------------
// EffectiveConfig
// ---------------------------------------------------------------------------

bool EffectiveConfig::use_color(bool tty) const {
  if (str::iequals(color, "always") || str::iequals(color, "yes") || color == "1" ||
      str::iequals(color, "true")) {
    return true;
  }
  if (str::iequals(color, "never") || str::iequals(color, "no") || color == "0" ||
      str::iequals(color, "false")) {
    return false;
  }
  if (str::iequals(color, "auto") || color.empty()) {
    return tty && !quiet;
  }
  // Unknown value: be conservative, never emit escapes into a non-tty.
  return tty;
}

int EffectiveConfig::resolved_jobs() const {
  if (jobs > 0) {
    return jobs;
  }
  const unsigned hardware = std::thread::hardware_concurrency();
  if (hardware == 0) {
    return 1;
  }
  return static_cast<int>(hardware);
}

EffectiveConfig merge(const UserConfig& user, const stdfs::path& data_dir,
                      const std::map<std::string, std::string, std::less<>>& cli_flags) {
  EffectiveConfig effective;
  effective.user = user;
  effective.data_dir = data_dir.empty() ? user.path.parent_path() : data_dir;
  effective.allow_install = user.install.allow_install;

  // CLI flags are spelled with hyphens (`--no-install`) while user-config keys
  // use underscores (`allow_install`). Treat `-` and `_` as the same separator
  // so a caller cannot silently miss a flag by picking the other spelling —
  // that mismatch previously made `--no-install` a no-op.
  auto flag = [&cli_flags](std::string_view key) -> std::optional<std::string> {
    auto normalize = [](std::string_view text) {
      std::string out(text);
      std::replace(out.begin(), out.end(), '-', '_');
      return out;
    };
    const std::string wanted = normalize(key);
    const auto it = cli_flags.find(key);
    if (it != cli_flags.end()) {
      return it->second;
    }
    for (const auto& [name, value] : cli_flags) {
      if (normalize(name) == wanted) {
        return value;
      }
    }
    return std::nullopt;
  };
  auto flag_bool = [&flag](std::string_view key, bool fallback) {
    const std::optional<std::string> text = flag(key);
    if (!text) {
      return fallback;
    }
    bool value = fallback;
    if (!parse_bool_text(*text, value)) {
      return true;  // presence of a boolean flag means "on"
    }
    return value;
  };
  auto flag_int = [&flag](std::string_view key, int fallback) {
    const std::optional<std::string> text = flag(key);
    if (!text) {
      return fallback;
    }
    std::int64_t value = 0;
    if (!parse_int_text(*text, value)) {
      return fallback;
    }
    return static_cast<int>(value);
  };

  effective.offline = flag_bool("offline", false);
  effective.locked = flag_bool("locked", false);
  effective.frozen = flag_bool("frozen", false);
  effective.verbose = flag_bool("verbose", false);
  effective.quiet = flag_bool("quiet", false);

  // `allow_install` is tri-state: an explicit --no-install wins, otherwise the
  // user config decides (PRD §7.2: the restriction must be nameable).
  if (const std::optional<std::string> install_flag = flag("allow_install"); install_flag) {
    bool value = effective.allow_install;
    if (parse_bool_text(*install_flag, value)) {
      effective.allow_install = value;
    }
  }
  if (flag_bool("no_install", false)) {
    effective.allow_install = false;
  }
  if (effective.offline) {
    // Offline implies no downloads, but it is reported as its own restriction.
    effective.allow_install = false;
  }
  if (user.cache.read_only) {
    effective.allow_install = false;
  }

  effective.jobs = flag_int("jobs", 0);
  if (effective.jobs < 0) {
    effective.jobs = 0;
  }

  if (const std::optional<std::string> color = flag("color"); color) {
    effective.color = *color;
  }

  if (const std::optional<std::string> data_dir_flag = flag("data_dir"); data_dir_flag) {
    effective.data_dir = stdfs::path(*data_dir_flag);
  }

  if (effective.quiet) {
    effective.verbose = false;
  }
  return effective;
}

}  // namespace cova::config
