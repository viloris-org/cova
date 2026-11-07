// Cova — Sysroot Manager (PRD §8).
//
// A sysroot is a described, verified component set: headers, CRT, C++ standard
// library, startup objects, link libraries, SDK metadata and ABI info. It is
// content-addressed in the immutable store and referenced by an id.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "core/version/version.hpp"
#include "platform/abi/abi.hpp"
#include "platform/triple/triple.hpp"

namespace cova::sysroot {

namespace stdfs = std::filesystem;

// The declared identity of a sysroot: what it is, not where it lives.
struct SysrootManifest {
  std::string id;                            // content-derived identity
  std::string name;                          // "aarch64-linux-musl"
  platform::Triple target;                   // canonical target triple
  platform::Crt crt = platform::Crt::Unknown;
  platform::LibCpp libcpp = platform::LibCpp::Unknown;
  platform::Abi abi = platform::Abi::Unknown;
  std::optional<platform::OsVersion> min_os_version;
  std::optional<Version> libc_version;
  std::string compiler_family;               // "clang" when compiler-specific
  std::string dynamic_linker;                // e.g. /lib/ld-musl-aarch64.so.1
  std::vector<std::string> include_dirs;     // relative to the sysroot root
  std::vector<std::string> lib_dirs;
  std::vector<std::string> crt_objects;
  std::string license;
  std::string source_url;
  std::string sha256;                        // of the archived component
  std::uint64_t size_bytes = 0;
  std::string version_text;
  std::vector<std::string> provides;         // logical components provided

  std::string to_string() const;
  // Serialises to the on-disk `sysroot.toml` form.
  std::string to_toml() const;
  static Result<SysrootManifest> from_toml(std::string_view text);
};

// Checks structural completeness of an installed sysroot tree: required include
// and lib directories, CRT objects, dynamic linker presence.
struct VerifyFinding {
  std::string check;
  bool passed = false;
  std::string detail;   // path or reason
};

struct VerifyReport {
  bool ok = false;
  std::vector<VerifyFinding> findings;
  std::string to_string() const;
};

Status verify(const stdfs::path& sysroot_root, const SysrootManifest& manifest,
              VerifyReport& out);

// An installed sysroot as the resolver sees it.
struct InstalledSysroot {
  SysrootManifest manifest;
  stdfs::path root;          // <sysroots>/<id>/root
  std::string content_hash;
  bool verified = false;
};

struct ManagerOptions {
  stdfs::path sysroots_root;
  stdfs::path store_root;
  stdfs::path downloads_root;
  stdfs::path registry_root;
  bool offline = false;
  bool allow_install = true;
  bool interactive = false;    // ask before large downloads (PRD §8.6)
  int timeout_ms = 60000;
};

class SysrootManager {
 public:
  explicit SysrootManager(ManagerOptions options);

  // Lists installed sysroots, newest first.
  Result<std::vector<InstalledSysroot>> list() const;
  // Finds an installed sysroot by id or by target match.
  Result<InstalledSysroot> find(std::string_view id) const;
  Result<std::vector<InstalledSysroot>> find_for_target(const platform::Triple& target) const;

  // Verifies every installed sysroot; used by `cova sysroot verify`.
  std::vector<std::pair<InstalledSysroot, VerifyReport>> verify_all() const;

  // Installs a sysroot for `target`, reusing an existing compatible install when
  // possible (PRD §8.6). Steps: resolve metadata → plan → download → verify hash
  // → safe unpack → atomic commit → verify compat → record.
  Result<InstalledSysroot> install(const platform::Triple& target,
                                   const std::string& sysroot_name);

  // Installs from a local archive, or from a cached registry entry when one is
  // present. This is the only install path that can verify what it installs in
  // this milestone; there is no public registry to download from (PRD §18.1).
  // `archive` may be empty to install from the registry cache.
  Result<InstalledSysroot> install_file(const platform::Triple& target, const std::string& name,
                                        std::optional<stdfs::path> archive = std::nullopt);

  // Removes an installed sysroot; refuses when other ids reference it.
  Status remove(std::string_view id);

  // Garbage collection over unreferenced store entries (PRD §12.4).
  struct GcPlan {
    std::vector<stdfs::path> removable;
    std::uint64_t reclaimable_bytes = 0;
  };
  Result<GcPlan> plan_gc() const;
  Status apply_gc(const GcPlan& plan) const;

 private:
  ManagerOptions options_;
};

// Safe archive extraction shared by sysroot and toolchain installation.
// Rejects absolute paths, "..", symlink escapes and special files (PRD §18.1).
struct ExtractLimits {
  std::uint64_t max_total_bytes = 4ull * 1024 * 1024 * 1024;
  std::size_t max_entries = 1000000;
  bool allow_symlinks = true;
};
Status extract_archive_safely(const stdfs::path& archive, const stdfs::path& dest_root,
                              const ExtractLimits& limits = {});

// Helper used by tests and by `cova sysroot install` for local archives.
Status install_from_archive(const stdfs::path& archive, const stdfs::path& sysroots_root,
                            const SysrootManifest& manifest);

}  // namespace cova::sysroot
