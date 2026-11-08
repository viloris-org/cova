// Cova — managed toolchain installation from a trusted local registry
// (PRD §7.2, §7.5, §8.6, §18.1).
//
// A registry entry is metadata plus a content-addressed archive. Cova verifies
// the archive hash before unpacking, validates the installed compiler's family
// and version, and only then commits the toolchain into the managed store.
// There is no network fetch in this build: an entry that points at a remote URL
// is reported as unsupported rather than guessed at.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "platform/triple/triple.hpp"
#include "toolchain/descriptor.hpp"

namespace cova::toolchain {

namespace stdfs = std::filesystem;

struct RegistryToolchainEntry {
  std::string id;
  std::string name;
  std::string family;
  std::string version;
  std::string target;  // canonical triple, or empty for host
  std::string archive; // path relative to the registry root, or file:// URL
  std::string sha256;
  std::string license;
  std::string source_url;
  std::string compiler; // path to the C++ driver inside the archive
  std::uint64_t size_bytes = 0;
  std::vector<std::string> provides;

  std::string to_string() const;
};

struct ToolchainInstallOptions {
  stdfs::path registry_root;
  stdfs::path toolchains_root;
  stdfs::path downloads_root;
  bool offline = false;
  bool allow_install = true;
  int timeout_ms = 120000;
};

struct InstalledToolchain {
  ToolchainDescriptor descriptor;
  stdfs::path root;
  RegistryToolchainEntry entry;
};

class ToolchainInstaller {
public:
  explicit ToolchainInstaller(ToolchainInstallOptions options);

  // Entries declared by `<registry>/index.toml` under `[[toolchain]]`.
  Result<std::vector<RegistryToolchainEntry>> available() const;

  // Installs by id (or by unique family/version/name match).
  Result<InstalledToolchain> install(std::string_view id);

  // Verifies that an already-installed toolchain still matches its entry.
  Result<InstalledToolchain> verify(std::string_view id) const;

private:
  ToolchainInstallOptions options_;
};

} // namespace cova::toolchain
