// Cova — layered configuration (PRD §5.4).
//
// Precedence (highest first):
//   1. explicit CLI options
//   2. project cova.toml
//   3. workspace cova.toml
//   4. user config (~/.cova/config.toml)
//   5. built-in defaults
//
// A lockfile is NOT a config layer: it constrains resolution results only.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/toml/toml.hpp"

namespace cova::config {

namespace stdfs = std::filesystem;

// [install]
struct InstallConfig {
  // Ask before downloads above this many bytes; 0 disables prompts.
  std::uint64_t confirm_threshold_bytes = 64ull * 1024 * 1024;
  bool allow_install = true;
};

// [cache]
struct CacheConfig {
  bool enabled = true;
  std::uint64_t max_size_bytes = 8ull * 1024 * 1024 * 1024;
  bool read_only = false;
};

// [network]
struct NetworkConfig {
  int timeout_ms = 60000;
  int retries = 2;
  // Trusted registries only, by design (PRD §18.1).
  std::vector<std::string> trusted_registries;
};

// [runtime]
struct RuntimeConfig {
  // Runner used when the target cannot execute natively (PRD §13.3).
  std::string default_runner;  // "", "native", "qemu", "wine", "remote"
  std::string qemu_prefix;     // e.g. "qemu-aarch64"
  bool prefer_native = true;
};

struct UserConfig {
  stdfs::path path;
  std::optional<stdfs::path> data_dir_override;
  InstallConfig install;
  CacheConfig cache;
  NetworkConfig network;
  RuntimeConfig runtime;
  // Free-form extra keys, for forward compatibility.
  toml::Table extras;

  std::string to_toml() const;
};

struct LoadOptions {
  // $COVA_HOME / XDG override is applied by fs::cova_dirs() before loading.
  std::optional<stdfs::path> config_path;
  bool require_file = false;  // treat a missing file as an error
};

// Loads the user config; a missing file yields defaults (not an error).
Result<UserConfig> load_user_config(const LoadOptions& options = {});
Status save_user_config(const UserConfig& config);

// Effective view after applying CLI overrides.
struct EffectiveConfig {
  UserConfig user;
  stdfs::path data_dir;
  bool offline = false;
  bool locked = false;
  bool frozen = false;
  bool allow_install = true;
  bool verbose = false;
  bool quiet = false;
  int jobs = 0;  // 0 = auto
  std::string color = "auto";

  bool use_color(bool tty) const;
  int resolved_jobs() const;
};

// Applies CLI flags on top of the user config.
EffectiveConfig merge(const UserConfig& user, const stdfs::path& data_dir,
                      const std::map<std::string, std::string, std::less<>>& cli_flags);

// Parses a user config file body (tests).
Result<UserConfig> parse_user_config(std::string_view text, std::string_view source_name,
                                     const stdfs::path& path);

}  // namespace cova::config
