// Cova — Sysroot Manager (PRD §8).
#include "sysroot/manager/manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/process/process.hpp"
#include "core/toml/toml.hpp"

namespace cova::sysroot {
namespace {

using toml::Table;
using toml::Value;

const Value* get(const Table& table, std::string_view key) {
  auto it = table.find(key);
  return it == table.end() ? nullptr : &it->second;
}

std::string string_or(const Table& table, std::string_view key, std::string fallback = {}) {
  const Value* value = get(table, key);
  return value != nullptr && value->is_string() ? value->as_string() : fallback;
}

std::vector<std::string> array_or(const Table& table, std::string_view key) {
  const Value* value = get(table, key);
  return value == nullptr ? std::vector<std::string>{} : value->as_string_array();
}

void put_array(Table& table, std::string_view key, const std::vector<std::string>& values) {
  if (values.empty()) {
    return;
  }
  toml::Array array;
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  table.emplace(std::string(key), Value(std::move(array)));
}

std::string lower(std::string_view text) { return str::to_lower(str::trim_view(text)); }

bool safe_relative_entry(std::string_view entry) {
  if (entry.empty()) {
    return false;
  }
  if (entry.front() == '/' || entry.front() == '\\') {
    return false;
  }
  if (entry.size() >= 2 && std::isalpha(static_cast<unsigned char>(entry[0])) && entry[1] == ':') {
    return false;
  }
  for (const auto& component : str::split(entry, '/')) {
    if (component == "..") {
      return false;
    }
  }
  for (const auto& component : str::split(entry, '\\')) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

Result<std::vector<std::string>> list_tar(const fs::Path& archive) {
  proc::ProcessSpec spec;
  spec.program = "tar";
  spec.args = {"-tf", fs::generic_string(archive)};
  spec.timeout = std::chrono::milliseconds(60000);
  auto result = proc::run(spec);
  if (!result || !result->success()) {
    return fail<std::vector<std::string>>(
        Error::make("cova::sysroot::archive", "could not list the tar archive"));
  }
  std::vector<std::string> entries;
  for (const auto& line : str::split(result->stdout_text, '\n')) {
    const std::string trimmed = str::trim(line);
    if (trimmed.empty()) {
      continue;
    }
    if (!safe_relative_entry(trimmed)) {
      return fail<std::vector<std::string>>(
          Error::make("cova::sysroot::unsafe_archive",
                      "archive entry escapes the extraction root: `" + trimmed + "`"));
    }
    entries.push_back(trimmed);
  }

  // Validate link targets before extraction. A symlink or hardlink whose target
  // escapes the root can be abused by a later entry written through it.
  proc::ProcessSpec verbose;
  verbose.program = "tar";
  verbose.args = {"-tvf", fs::generic_string(archive)};
  verbose.timeout = std::chrono::milliseconds(60000);
  auto details = proc::run(verbose);
  if (details && details->success()) {
    for (const auto& line : str::split(details->stdout_text, '\n')) {
      if (line.empty() || (line.front() != 'l' && line.front() != 'h')) {
        continue;
      }
      const auto arrow = line.find(" -> ");
      if (arrow == std::string::npos) {
        return fail<std::vector<std::string>>(
            Error::make("cova::sysroot::unsafe_archive", "archive link entry has no target"));
      }
      const std::string target = str::trim(line.substr(arrow + 4));
      if (!safe_relative_entry(target)) {
        return fail<std::vector<std::string>>(
            Error::make("cova::sysroot::unsafe_archive",
                        "archive link target escapes the extraction root: `" + target + "`"));
      }
    }
  }
  return entries;
}

Result<std::vector<std::string>> list_zip(const fs::Path& archive) {
  proc::ProcessSpec spec;
  spec.program = "unzip";
  spec.args = {"-Z1", fs::generic_string(archive)};
  spec.timeout = std::chrono::milliseconds(60000);
  auto result = proc::run(spec);
  if (!result || !result->success()) {
    return fail<std::vector<std::string>>(
        Error::make("cova::sysroot::archive", "could not list the zip archive"));
  }
  std::vector<std::string> entries;
  for (const auto& line : str::split(result->stdout_text, '\n')) {
    const std::string trimmed = str::trim(line);
    if (trimmed.empty()) {
      continue;
    }
    if (!safe_relative_entry(trimmed)) {
      return fail<std::vector<std::string>>(
          Error::make("cova::sysroot::unsafe_archive",
                      "archive entry escapes the extraction root: `" + trimmed + "`"));
    }
    entries.push_back(trimmed);
  }

  // Validate link targets before extraction. A symlink or hardlink whose target
  // escapes the root can be abused by a later entry written through it.
  proc::ProcessSpec verbose;
  verbose.program = "tar";
  verbose.args = {"-tvf", fs::generic_string(archive)};
  verbose.timeout = std::chrono::milliseconds(60000);
  auto details = proc::run(verbose);
  if (details && details->success()) {
    for (const auto& line : str::split(details->stdout_text, '\n')) {
      if (line.empty() || (line.front() != 'l' && line.front() != 'h')) {
        continue;
      }
      const auto arrow = line.find(" -> ");
      if (arrow == std::string::npos) {
        return fail<std::vector<std::string>>(
            Error::make("cova::sysroot::unsafe_archive", "archive link entry has no target"));
      }
      const std::string target = str::trim(line.substr(arrow + 4));
      if (!safe_relative_entry(target)) {
        return fail<std::vector<std::string>>(
            Error::make("cova::sysroot::unsafe_archive",
                        "archive link target escapes the extraction root: `" + target + "`"));
      }
    }
  }
  return entries;
}

Status extract_tar(const fs::Path& archive, const fs::Path& dest) {
  proc::ProcessSpec spec;
  spec.program = "tar";
  spec.args = {"-xf",
               fs::generic_string(archive),
               "-C",
               fs::generic_string(dest),
               "--no-same-owner",
               "--no-same-permissions"};
  spec.timeout = std::chrono::milliseconds(120000);
  auto result = proc::run(spec);
  if (!result || !result->success()) {
    return fail(Error::make("cova::sysroot::archive", "could not extract the tar archive")
                    .with(result ? str::trim(result->stderr_text) : ""));
  }
  return ok();
}

Status extract_zip(const fs::Path& archive, const fs::Path& dest) {
  proc::ProcessSpec spec;
  spec.program = "unzip";
  spec.args = {"-q", fs::generic_string(archive), "-d", fs::generic_string(dest)};
  spec.timeout = std::chrono::milliseconds(120000);
  auto result = proc::run(spec);
  if (!result || !result->success()) {
    return fail(Error::make("cova::sysroot::archive", "could not extract the zip archive")
                    .with(result ? str::trim(result->stderr_text) : ""));
  }
  return ok();
}

bool is_zip(const fs::Path& archive) {
  const std::string extension = lower(archive.extension().string());
  return extension == ".zip";
}

std::optional<fs::Path> find_manifest_root(const fs::Path& extracted) {
  if (fs::is_regular_file(extracted / "sysroot.toml")) {
    return extracted;
  }
  // Search a small number of levels for a bundled manifest.
  for (const auto& file : fs::walk_files(extracted)) {
    if (file.filename() == "sysroot.toml") {
      return file.parent_path();
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Trusted local registry (PRD §8.6, §18.1)
// ---------------------------------------------------------------------------
//
// A registry is a directory with an `index.toml` and content-addressed archive
// files. Cova never installs an archive whose SHA-256 does not match the index;
// an entry without a hash is a malformed registry, not a "trust me" shortcut.

struct RegistrySysrootEntry {
  std::string id;
  std::string name;
  std::string target;
  std::string crt;
  std::string libcpp;
  std::string archive;
  std::string sha256;
  std::string license;
  std::string source_url;
  std::uint64_t size_bytes = 0;
  std::vector<std::string> provides;
};

Result<std::vector<RegistrySysrootEntry>> load_registry_entries(const fs::Path& registry_root) {
  std::vector<RegistrySysrootEntry> entries;
  const auto index = registry_root / "index.toml";
  if (!fs::is_regular_file(index)) {
    return entries;
  }
  auto document = toml::parse_file(fs::generic_string(index));
  if (!document || !document->is_table()) {
    return fail<std::vector<RegistrySysrootEntry>>(
        Error::make("cova::registry::invalid", "registry index is not a valid TOML table")
            .with("path: " + fs::generic_string(index)));
  }
  const auto& table = *document->as_table();
  auto array = table.find("sysroot");
  if (array == table.end() || !array->second.is_array()) {
    return entries;
  }
  for (const auto& value : *array->second.as_array()) {
    const auto* item = value.as_table();
    if (item == nullptr) {
      continue;
    }
    RegistrySysrootEntry entry;
    entry.id = string_or(*item, "id");
    entry.name = string_or(*item, "name");
    entry.target = string_or(*item, "target");
    entry.crt = string_or(*item, "crt");
    entry.libcpp = string_or(*item, "libcpp");
    entry.archive = string_or(*item, "archive");
    entry.sha256 = lower(string_or(*item, "sha256"));
    entry.license = string_or(*item, "license");
    entry.source_url = string_or(*item, "source");
    entry.provides = array_or(*item, "provides");
    if (auto size = get(*item, "size-bytes");
        size != nullptr && size->is_integer() && size->as_integer() > 0) {
      entry.size_bytes = static_cast<std::uint64_t>(size->as_integer());
    }
    if (entry.name.empty()) {
      entry.name = entry.id;
    }
    if (entry.id.empty()) {
      entry.id = entry.name;
    }
    if (entry.target.empty() || entry.archive.empty() || entry.sha256.empty()) {
      return fail<std::vector<RegistrySysrootEntry>>(
          Error::make("cova::registry::invalid", "registry sysroot entry `" + entry.name +
                                                     "` must declare target, archive and sha256"));
    }
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(),
            [](const RegistrySysrootEntry& a, const RegistrySysrootEntry& b) {
              if (a.target != b.target) {
                return a.target < b.target;
              }
              return a.name < b.name;
            });
  return entries;
}

Result<fs::Path> registry_archive_path(const fs::Path& registry_root, std::string_view archive) {
  std::string text = str::trim(archive);
  if (str::starts_with(text, "http://") || str::starts_with(text, "https://") ||
      str::starts_with(text, "ftp://")) {
    return fail<fs::Path>(
        Error::make("cova::registry::download_unsupported",
                    "this build cannot download sysroot archives over the network")
            .with("archive: " + text)
            .with("mirror the archive into the local registry and reference it by relative path"));
  }
  if (str::starts_with(text, "file://")) {
    text = text.substr(7);
  }
  fs::Path path(text);
  if (!path.is_absolute()) {
    const fs::Path root = fs::absolute_normalized(registry_root);
    path = fs::absolute_normalized(root / path);
    if (!fs::is_within(path, root)) {
      return fail<fs::Path>(
          Error::make("cova::registry::unsafe_path",
                      "registry archive path escapes the registry root: `" + text + "`"));
    }
  } else {
    path = fs::absolute_normalized(path);
  }
  if (!fs::is_regular_file(path) && !fs::is_directory(path)) {
    return fail<fs::Path>(
        Error::make("cova::registry::archive_missing",
                    "registry archive does not exist: " + fs::generic_string(path)));
  }
  return path;
}

// Copies an archive into the download cache and verifies its content hash. The
// cached file is named by hash, so the same content is never fetched twice.
Result<fs::Path> stage_verified_archive_impl(const fs::Path& downloads_root,
                                             const fs::Path& archive,
                                             std::string_view expected_sha256,
                                             std::uint64_t expected_size) {
  if (!fs::is_regular_file(archive)) {
    return fail<fs::Path>(
        Error::make("cova::registry::archive_missing",
                    "registry archive is not a file: " + fs::generic_string(archive)));
  }
  if (expected_size != 0 && fs::file_size(archive) != expected_size) {
    return fail<fs::Path>(Error::make("cova::registry::verification_failed",
                                      "archive size does not match the registry entry")
                              .with("expected: " + std::to_string(expected_size) + " bytes")
                              .with("found: " + std::to_string(fs::file_size(archive)) + " bytes"));
  }
  auto digest = Sha256::hex_of_file(archive);
  if (!digest) {
    return fail<fs::Path>(digest.error());
  }
  if (!str::iequals(*digest, expected_sha256)) {
    return fail<fs::Path>(Error::make("cova::registry::verification_failed",
                                      "archive checksum does not match the registry entry")
                              .with("expected: " + std::string(expected_sha256))
                              .with("found: " + *digest));
  }
  auto created = fs::create_directories(downloads_root);
  if (!created) {
    return fail<fs::Path>(created.error());
  }
  const auto cached = downloads_root / (str::to_lower(*digest) + "-" + archive.filename().string());
  if (fs::is_regular_file(cached)) {
    auto cached_digest = Sha256::hex_of_file(cached);
    if (cached_digest && str::iequals(*cached_digest, expected_sha256)) {
      return cached;
    }
  }
  auto copied = fs::copy_file(archive, cached, true);
  if (!copied) {
    return fail<fs::Path>(copied.error());
  }
  auto copied_digest = Sha256::hex_of_file(cached);
  if (!copied_digest || !str::iequals(*copied_digest, expected_sha256)) {
    auto cleanup = fs::remove_file(cached);
    (void)cleanup;
    return fail<fs::Path>(Error::make("cova::registry::verification_failed",
                                      "the staged archive failed verification after copying"));
  }
  return cached;
}

std::uint64_t tree_size(const fs::Path& root) {
  std::uint64_t total = 0;
  std::error_code ec;
  if (!fs::is_directory(root)) {
    return 0;
  }
  for (auto it = stdfs::recursive_directory_iterator(root, ec);
       it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      total += it->file_size(ec);
    }
  }
  return total;
}

Status validate_extracted_tree(const fs::Path& dest, const ExtractLimits& limits) {
  std::size_t entries = 0;
  std::uint64_t total = 0;
  std::error_code ec;
  for (auto it = stdfs::recursive_directory_iterator(dest, ec);
       it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
    ++entries;
    if (entries > limits.max_entries) {
      return fail(
          Error::make("cova::sysroot::unsafe_archive", "archive contains too many entries"));
    }
    const auto status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (stdfs::is_symlink(status)) {
      const auto target = stdfs::read_symlink(it->path(), ec);
      if (ec) {
        return fail(Error::make("cova::sysroot::unsafe_archive",
                                "archive contains a symlink that cannot be read"));
      }
      if (target.is_absolute()) {
        return fail(
            Error::make("cova::sysroot::unsafe_archive", "archive contains an absolute symlink"));
      }
      const auto resolved = fs::normalize(it->path().parent_path() / target);
      if (!fs::is_within(resolved, dest)) {
        return fail(Error::make("cova::sysroot::unsafe_archive",
                                "archive contains a symlink that escapes the sysroot"));
      }
      if (!limits.allow_symlinks) {
        return fail(Error::make("cova::sysroot::unsafe_archive",
                                "archive contains symlinks but they are not permitted"));
      }
    } else if (stdfs::is_regular_file(status)) {
      total += it->file_size(ec);
      if (total > limits.max_total_bytes) {
        return fail(Error::make("cova::sysroot::unsafe_archive",
                                "archive expands beyond the configured size limit"));
      }
    } else if (stdfs::is_directory(status)) {
      // Directories are fine.
    } else {
      return fail(Error::make("cova::sysroot::unsafe_archive", "archive contains a special file"));
    }
  }
  return ok();
}

Result<InstalledSysroot> install_tree(const fs::Path& source_root, const SysrootManifest& manifest,
                                      const fs::Path& sysroots_root, const fs::Path& allowed_root) {
  if (!fs::is_directory(source_root)) {
    return fail<InstalledSysroot>(
        Error::make("cova::sysroot::invalid", "the sysroot tree does not exist"));
  }
  const std::string id = manifest.id.empty() ? manifest.name : manifest.id;
  if (id.empty()) {
    return fail<InstalledSysroot>(
        Error::make("cova::sysroot::invalid", "the sysroot manifest has no id or name"));
  }
  const auto destination = sysroots_root / id;
  if (fs::is_directory(destination)) {
    InstalledSysroot existing;
    existing.manifest = manifest;
    existing.root = destination / "root";
    existing.verified = true;
    return existing;
  }
  auto created = fs::create_directories(destination);
  if (!created) {
    return fail<InstalledSysroot>(created.error());
  }
  auto copied = fs::copy_tree(source_root, destination / "root");
  if (!copied) {
    auto cleanup = fs::remove_tree(destination, allowed_root);
    (void)cleanup;
    return fail<InstalledSysroot>(copied.error());
  }
  SysrootManifest stored = manifest;
  if (stored.id.empty()) {
    stored.id = id;
  }
  auto manifest_written = fs::write_file_atomic(destination / "sysroot.toml", stored.to_toml());
  if (!manifest_written) {
    auto cleanup = fs::remove_tree(destination, allowed_root);
    (void)cleanup;
    return fail<InstalledSysroot>(manifest_written.error());
  }
  // A copy inside the tree helps tools that inspect the sysroot directly.
  auto nested = fs::write_file_atomic(destination / "root" / "sysroot.toml", stored.to_toml());
  (void)nested;

  VerifyReport report;
  auto verified = verify(destination / "root", stored, report);
  InstalledSysroot installed;
  installed.manifest = stored;
  installed.root = destination / "root";
  installed.content_hash = Sha256::hex_of_file(destination / "sysroot.toml").value_or("");
  installed.verified = verified.has_value() && report.ok;
  return installed;
}

std::string crt_text(platform::Crt crt) { return std::string(platform::to_string(crt)); }

} // namespace

Result<stdfs::path> stage_verified_archive(const stdfs::path& downloads_root,
                                           const stdfs::path& archive,
                                           std::string_view expected_sha256,
                                           std::uint64_t expected_size) {
  return stage_verified_archive_impl(downloads_root, archive, expected_sha256, expected_size);
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
std::string SysrootManifest::to_string() const {
  return name + " (" + target.to_string() + ", " + crt_text(crt) + ")";
}

std::string SysrootManifest::to_toml() const {
  Table table;
  table.emplace("id", Value(id));
  table.emplace("name", Value(name));
  table.emplace("target", Value(target.to_string()));
  table.emplace("crt", Value(crt_text(crt)));
  table.emplace("libcpp", Value(std::string(platform::to_string(libcpp))));
  table.emplace("abi", Value(std::string(platform::to_string(abi))));
  if (min_os_version) {
    table.emplace("min-os-version", Value(min_os_version->to_string()));
  }
  if (libc_version) {
    table.emplace("libc-version", Value(libc_version->to_string()));
  }
  if (!compiler_family.empty()) {
    table.emplace("compiler-family", Value(compiler_family));
  }
  if (!dynamic_linker.empty()) {
    table.emplace("dynamic-linker", Value(dynamic_linker));
  }
  put_array(table, "include-dirs", include_dirs);
  put_array(table, "lib-dirs", lib_dirs);
  put_array(table, "crt-objects", crt_objects);
  put_array(table, "provides", provides);
  if (!license.empty()) {
    table.emplace("license", Value(license));
  }
  if (!source_url.empty()) {
    table.emplace("source-url", Value(source_url));
  }
  table.emplace("sha256", Value(sha256));
  table.emplace("size-bytes", Value(static_cast<std::int64_t>(size_bytes)));
  if (!version_text.empty()) {
    table.emplace("version", Value(version_text));
  }
  return toml::write(table);
}

Result<SysrootManifest> SysrootManifest::from_toml(std::string_view text) {
  auto document = toml::parse(text, "sysroot.toml");
  if (!document || !document->is_table()) {
    return fail<SysrootManifest>(
        Error::make("cova::sysroot::manifest", "sysroot.toml is not a valid TOML table"));
  }
  const Table& table = *document->as_table();
  SysrootManifest manifest;
  manifest.id = string_or(table, "id");
  manifest.name = string_or(table, "name");
  const std::string target_text = string_or(table, "target");
  if (target_text.empty()) {
    return fail<SysrootManifest>(
        Error::make("cova::sysroot::manifest", "sysroot.toml is missing `target`"));
  }
  auto target = platform::Triple::parse(target_text);
  if (!target) {
    return fail<SysrootManifest>(
        Error::make("cova::sysroot::manifest", "sysroot.toml has an invalid target")
            .with(target.error().message));
  }
  manifest.target = *target;
  if (auto value = get(table, "min-os-version"); value != nullptr && value->is_string()) {
    if (auto parsed = platform::OsVersion::parse(value->as_string())) {
      manifest.min_os_version = *parsed;
    }
  }
  if (auto value = get(table, "libc-version"); value != nullptr && value->is_string()) {
    if (auto parsed = Version::parse(value->as_string())) {
      manifest.libc_version = *parsed;
    }
  }
  const std::string crt = lower(string_or(table, "crt"));
  if (crt == "glibc")
    manifest.crt = platform::Crt::Glibc;
  else if (crt == "musl")
    manifest.crt = platform::Crt::Musl;
  else if (crt == "uclibc")
    manifest.crt = platform::Crt::Uclibc;
  else if (crt == "msvc")
    manifest.crt = platform::Crt::Msvc;
  else if (crt == "wasi")
    manifest.crt = platform::Crt::WasmWasi;
  else if (crt == "none")
    manifest.crt = platform::Crt::None;
  const std::string libcpp = lower(string_or(table, "libcpp"));
  if (libcpp == "libstdc++")
    manifest.libcpp = platform::LibCpp::LibStdCpp;
  else if (libcpp == "libc++")
    manifest.libcpp = platform::LibCpp::LibCpp;
  else if (libcpp == "msvc-stl")
    manifest.libcpp = platform::LibCpp::MsvcStl;
  else if (libcpp == "none")
    manifest.libcpp = platform::LibCpp::NoStd;
  const std::string abi = lower(string_or(table, "abi"));
  if (abi == "sysv")
    manifest.abi = platform::Abi::SysV;
  else if (abi == "msvc")
    manifest.abi = platform::Abi::Msvc;
  else if (abi == "musl")
    manifest.abi = platform::Abi::Musl;
  else if (abi == "gnueabi")
    manifest.abi = platform::Abi::Gnueabi;
  else if (abi == "gnueabihf")
    manifest.abi = platform::Abi::Gnueabihf;
  else if (abi == "wasm")
    manifest.abi = platform::Abi::Wasm;
  else if (abi == "eabi")
    manifest.abi = platform::Abi::Eabi;
  else if (abi == "macho")
    manifest.abi = platform::Abi::MachO;
  manifest.compiler_family = string_or(table, "compiler-family");
  manifest.dynamic_linker = string_or(table, "dynamic-linker");
  manifest.include_dirs = array_or(table, "include-dirs");
  manifest.lib_dirs = array_or(table, "lib-dirs");
  manifest.crt_objects = array_or(table, "crt-objects");
  manifest.provides = array_or(table, "provides");
  manifest.license = string_or(table, "license");
  manifest.source_url = string_or(table, "source-url");
  manifest.sha256 = string_or(table, "sha256");
  if (auto value = get(table, "size-bytes"); value != nullptr && value->is_integer()) {
    manifest.size_bytes = static_cast<std::uint64_t>(value->as_integer());
  }
  manifest.version_text = string_or(table, "version");
  if (manifest.id.empty() && !manifest.name.empty()) {
    manifest.id = manifest.name;
  }
  return manifest;
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------
Status verify(const fs::Path& sysroot_root, const SysrootManifest& manifest, VerifyReport& out) {
  out.ok = true;
  auto add = [&](std::string check, bool passed, std::string detail) {
    out.findings.push_back(VerifyFinding{std::move(check), passed, std::move(detail)});
    if (!passed) {
      out.ok = false;
    }
  };
  if (!fs::is_directory(sysroot_root)) {
    add("sysroot root", false, "missing directory " + fs::generic_string(sysroot_root));
    return ok();
  }
  add("sysroot root", true, fs::generic_string(sysroot_root));
  for (const auto& dir : manifest.include_dirs) {
    add("include directory", fs::is_directory(sysroot_root / dir),
        fs::generic_string(sysroot_root / dir));
  }
  for (const auto& dir : manifest.lib_dirs) {
    add("library directory", fs::is_directory(sysroot_root / dir),
        fs::generic_string(sysroot_root / dir));
  }
  for (const auto& object : manifest.crt_objects) {
    const auto path = sysroot_root / object;
    add("crt object", fs::is_regular_file(path) || fs::is_symlink(path), fs::generic_string(path));
  }
  if (!manifest.dynamic_linker.empty()) {
    fs::Path linker = manifest.dynamic_linker;
    if (linker.is_absolute()) {
      linker = sysroot_root / fs::Path(fs::generic_string(linker).substr(1));
    } else {
      linker = sysroot_root / linker;
    }
    add("dynamic linker", fs::is_regular_file(linker) || fs::is_symlink(linker),
        fs::generic_string(linker));
  }
  return ok();
}

std::string VerifyReport::to_string() const {
  std::string out;
  for (const auto& finding : findings) {
    out += finding.passed ? "  ok   " : "  FAIL ";
    out += finding.check;
    out += "  ";
    out += finding.detail;
    out += "\n";
  }
  return out;
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------
SysrootManager::SysrootManager(ManagerOptions options) : options_(std::move(options)) {}

Result<std::vector<InstalledSysroot>> SysrootManager::list() const {
  std::vector<InstalledSysroot> out;
  if (!fs::is_directory(options_.sysroots_root)) {
    return out;
  }
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(options_.sysroots_root, ec);
       it != stdfs::directory_iterator(); it.increment(ec)) {
    if (!it->is_directory()) {
      continue;
    }
    const auto manifest_path = it->path() / "sysroot.toml";
    if (!fs::is_regular_file(manifest_path)) {
      continue;
    }
    auto text = fs::read_file(manifest_path);
    if (!text) {
      continue;
    }
    auto manifest = SysrootManifest::from_toml(*text);
    if (!manifest) {
      continue;
    }
    InstalledSysroot installed;
    installed.manifest = std::move(*manifest);
    installed.root = it->path() / "root";
    if (!fs::is_directory(installed.root)) {
      installed.root = it->path();
    }
    installed.content_hash = Sha256::hex_of_file(manifest_path).value_or("");
    VerifyReport report;
    auto status = verify(installed.root, installed.manifest, report);
    installed.verified = status.has_value() && report.ok;
    out.push_back(std::move(installed));
  }
  std::sort(out.begin(), out.end(), [](const InstalledSysroot& a, const InstalledSysroot& b) {
    return a.manifest.id > b.manifest.id;
  });
  return out;
}

Result<InstalledSysroot> SysrootManager::find(std::string_view id) const {
  auto all = list();
  if (!all) {
    return fail<InstalledSysroot>(all.error());
  }
  for (const auto& entry : *all) {
    if (entry.manifest.id == id || entry.manifest.name == id ||
        entry.root.parent_path().filename().string() == id) {
      return entry;
    }
  }
  return fail<InstalledSysroot>(Error::make(
      "cova::sysroot::not_found", "no installed sysroot named `" + std::string(id) + "`"));
}

Result<std::vector<InstalledSysroot>>
SysrootManager::find_for_target(const platform::Triple& target) const {
  std::vector<InstalledSysroot> out;
  auto all = list();
  if (!all) {
    return fail<std::vector<InstalledSysroot>>(all.error());
  }
  for (const auto& entry : *all) {
    if (entry.manifest.target.arch == target.arch && entry.manifest.target.os == target.os &&
        (entry.manifest.crt == platform::Crt::Unknown || entry.manifest.crt == target.crt())) {
      out.push_back(entry);
    }
  }
  return out;
}

std::vector<std::pair<InstalledSysroot, VerifyReport>> SysrootManager::verify_all() const {
  std::vector<std::pair<InstalledSysroot, VerifyReport>> out;
  auto all = list();
  if (!all) {
    return out;
  }
  for (auto entry : *all) {
    VerifyReport report;
    auto status = verify(entry.root, entry.manifest, report);
    if (!status) {
      report.ok = false;
      report.findings.push_back(VerifyFinding{"verification", false, status.error().message});
    }
    out.emplace_back(std::move(entry), std::move(report));
  }
  return out;
}

Result<InstalledSysroot> SysrootManager::install(const platform::Triple& target,
                                                 const std::string& sysroot_name) {
  auto entries = load_registry_entries(options_.registry_root);
  if (!entries) {
    return fail<InstalledSysroot>(entries.error());
  }

  std::vector<RegistrySysrootEntry> matching;
  for (const auto& entry : *entries) {
    const bool name_matches =
        sysroot_name.empty() || entry.name == sysroot_name || entry.id == sysroot_name;
    if (!name_matches) {
      continue;
    }
    auto parsed = platform::Triple::parse(entry.target);
    if (!parsed || parsed->arch != target.arch || parsed->os != target.os) {
      continue;
    }
    if (parsed->env != target.env && parsed->env != platform::Env::Unknown) {
      continue;
    }
    if (!entry.crt.empty()) {
      const std::string wanted = lower(entry.crt);
      if (wanted != "unknown" && wanted != lower(std::string(platform::to_string(target.crt())))) {
        continue;
      }
    }
    matching.push_back(entry);
  }

  if (!matching.empty()) {
    const RegistrySysrootEntry& entry = matching.front();
    if (options_.offline) {
      return fail<InstalledSysroot>(
          Error::make("cova::registry::offline",
                      "offline mode forbids downloading the sysroot `" + entry.name + "`")
              .with("target: " + target.to_string())
              .with("the component is not installed locally"));
    }
    if (!options_.allow_install) {
      return fail<InstalledSysroot>(
          Error::make("cova::registry::install_disabled",
                      "installation is disabled, so the sysroot `" + entry.name +
                          "` cannot be installed")
              .with("target: " + target.to_string())
              .with("remove --no-install / offline restrictions to allow it"));
    }
    auto archive = registry_archive_path(options_.registry_root, entry.archive);
    if (!archive) {
      return fail<InstalledSysroot>(archive.error());
    }
    auto staged = stage_verified_archive_impl(options_.downloads_root, *archive, entry.sha256,
                                              entry.size_bytes);
    if (!staged) {
      return fail<InstalledSysroot>(staged.error());
    }
    auto installed = install_file(target, entry.name, *staged);
    if (!installed) {
      return installed;
    }
    // The registry's metadata is authoritative for the identity Cova records.
    if (installed->manifest.crt == platform::Crt::Unknown) {
      installed->manifest.crt = target.crt();
    }
    if (installed->manifest.libcpp == platform::LibCpp::Unknown) {
      installed->manifest.libcpp = target.default_libcpp();
    }
    return installed;
  }

  // A manually populated `registry/sysroots/<name>` cache is still supported:
  // the archive is hashed and verified by `install_file`, just without an
  // index entry to compare against.
  const fs::Path registry_candidate =
      sysroot_name.empty() ? fs::Path{} : options_.registry_root / "sysroots" / sysroot_name;
  if (!registry_candidate.empty() && fs::is_directory(registry_candidate)) {
    for (const auto& file : fs::walk_files(registry_candidate)) {
      const std::string extension = lower(file.extension().string());
      if (extension == ".tar" || extension == ".gz" || extension == ".tgz" || extension == ".zip") {
        return install_file(target, sysroot_name, file);
      }
    }
  }
  return fail<InstalledSysroot>(
      Error::make("cova::sysroot::registry_unavailable",
                  "no trusted registry entry is available for this sysroot")
          .with("requested: " + target.to_string() +
                (sysroot_name.empty() ? "" : " / " + sysroot_name))
          .with("registry: " + fs::generic_string(options_.registry_root))
          .with("Cova will not simulate a download or claim an install it did "
                "not verify "
                "(PRD §18.1)")
          .with("add an `[[sysroot]]` entry to the registry index, or install "
                "from a local "
                "archive with `cova sysroot install --from <archive>`"));
}

Result<InstalledSysroot> SysrootManager::install_file(const platform::Triple& target,
                                                      const std::string& name,
                                                      std::optional<fs::Path> archive) {
  fs::Path archive_path;
  if (archive) {
    archive_path = *archive;
  } else {
    // Reuse `install`'s cached registry lookup when no archive is supplied.
    const fs::Path registry_candidate = options_.registry_root / "sysroots" / name;
    if (fs::is_directory(registry_candidate)) {
      for (const auto& file : fs::walk_files(registry_candidate)) {
        const std::string extension = lower(file.extension().string());
        if (extension == ".tar" || extension == ".gz" || extension == ".tgz" ||
            extension == ".zip") {
          archive_path = file;
          break;
        }
      }
    }
    if (archive_path.empty()) {
      return fail<InstalledSysroot>(
          Error::make("cova::sysroot::registry_unavailable",
                      "no local archive or registry cache entry is available "
                      "for this sysroot")
              .with("requested: " + target.to_string() + " / " + name));
    }
  }
  if (!fs::is_regular_file(archive_path) && !fs::is_directory(archive_path)) {
    return fail<InstalledSysroot>(
        Error::make("cova::sysroot::archive_missing",
                    "archive " + fs::generic_string(archive_path) + " does not exist"));
  }

  const auto parent =
      options_.downloads_root.empty() ? options_.sysroots_root : options_.downloads_root;
  auto created = fs::create_directories(parent);
  if (!created) {
    return fail<InstalledSysroot>(created.error());
  }
  auto scratch = fs::make_temp_dir(parent, "cova-sysroot-");
  if (!scratch) {
    return fail<InstalledSysroot>(scratch.error());
  }

  fs::Path extracted = *scratch / "extracted";
  auto extracted_dirs = fs::create_directories(extracted);
  if (!extracted_dirs) {
    return fail<InstalledSysroot>(extracted_dirs.error());
  }

  if (fs::is_directory(archive_path)) {
    extracted = archive_path;
  } else {
    ExtractLimits limits;
    Status status = ok();
    if (is_zip(archive_path)) {
      auto entries = list_zip(archive_path);
      if (!entries) {
        status = fail(entries.error());
      } else if (entries->size() > limits.max_entries) {
        status = fail(Error::make("cova::sysroot::unsafe_archive", "archive has too many entries"));
      } else {
        status = extract_zip(archive_path, extracted);
      }
    } else {
      auto entries = list_tar(archive_path);
      if (!entries) {
        status = fail(entries.error());
      } else if (entries->size() > limits.max_entries) {
        status = fail(Error::make("cova::sysroot::unsafe_archive", "archive has too many entries"));
      } else {
        status = extract_tar(archive_path, extracted);
      }
    }
    if (!status) {
      auto cleanup = fs::remove_tree(*scratch, parent);
      (void)cleanup;
      return fail<InstalledSysroot>(status.error());
    }
    auto validated = validate_extracted_tree(extracted, limits);
    if (!validated) {
      auto cleanup = fs::remove_tree(*scratch, parent);
      (void)cleanup;
      return fail<InstalledSysroot>(validated.error());
    }
  }

  SysrootManifest manifest;
  bool have_manifest = false;
  if (auto manifest_root = find_manifest_root(extracted)) {
    auto text = fs::read_file(*manifest_root / "sysroot.toml");
    if (text) {
      auto parsed = SysrootManifest::from_toml(*text);
      if (parsed) {
        manifest = std::move(*parsed);
        have_manifest = true;
        extracted = *manifest_root;
      }
    }
  }
  if (!have_manifest) {
    manifest.name = name;
    manifest.target = target;
    manifest.crt = target.crt();
    manifest.libcpp = target.default_libcpp();
    manifest.abi = target.abi();
    manifest.id = name + "-" + Sha256::hex_of_file(archive_path).value_or("local").substr(0, 12);
    if (fs::is_directory(extracted / "usr" / "include")) {
      manifest.include_dirs.push_back("usr/include");
    } else if (fs::is_directory(extracted / "include")) {
      manifest.include_dirs.push_back("include");
    }
    if (fs::is_directory(extracted / "usr" / "lib")) {
      manifest.lib_dirs.push_back("usr/lib");
    } else if (fs::is_directory(extracted / "lib")) {
      manifest.lib_dirs.push_back("lib");
    }
  }

  const bool environment_mismatch =
      manifest.target.env != platform::Env::Unknown && target.env != platform::Env::Unknown &&
      manifest.target.env != target.env;
  const bool runtime_mismatch =
      manifest.crt != platform::Crt::Unknown && manifest.crt != target.crt();
  if (manifest.target.arch != target.arch || manifest.target.os != target.os ||
      environment_mismatch || runtime_mismatch) {
    auto cleanup = fs::remove_tree(*scratch, parent);
    (void)cleanup;
    return fail<InstalledSysroot>(Error::make("cova::sysroot::incompatible",
                                              "the archive targets " + manifest.target.to_string() +
                                                  " but " + target.to_string() + " was requested")
                                      .with("archive crt: " +
                                            std::string(platform::to_string(manifest.crt)))
                                      .with("requested crt: " +
                                            std::string(platform::to_string(target.crt()))));
  }
  if (!manifest.id.empty()) {
    // preserve the manifest identity
  } else {
    manifest.id = name;
  }
  if (manifest.name.empty()) {
    manifest.name = name;
  }
  if (fs::is_regular_file(archive_path)) {
    const std::string digest = Sha256::hex_of_file(archive_path).value_or("");
    manifest.sha256 = digest;
    manifest.size_bytes = fs::file_size(archive_path);
  }

  auto installed =
      install_tree(extracted, manifest, options_.sysroots_root, options_.sysroots_root);
  if (fs::generic_string(*scratch) != fs::generic_string(archive_path)) {
    auto cleanup = fs::remove_tree(*scratch, parent);
    (void)cleanup;
  }
  return installed;
}

Status SysrootManager::remove(std::string_view id) {
  auto found = find(id);
  if (!found) {
    return fail(found.error());
  }
  const auto directory = found->root.parent_path();
  auto removed = fs::remove_tree(directory, options_.sysroots_root);
  if (!removed) {
    return fail(removed.error());
  }
  return ok();
}

Result<SysrootManager::GcPlan> SysrootManager::plan_gc() const {
  GcPlan plan;
  if (!fs::is_directory(options_.store_root)) {
    return plan;
  }
  auto installed = list();
  std::vector<std::string> referenced;
  if (installed) {
    for (const auto& entry : *installed) {
      referenced.push_back(entry.manifest.id);
    }
  }
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(options_.store_root, ec);
       it != stdfs::directory_iterator(); it.increment(ec)) {
    if (!it->is_directory()) {
      continue;
    }
    const std::string name = it->path().filename().string();
    if (std::find(referenced.begin(), referenced.end(), name) == referenced.end()) {
      plan.removable.push_back(it->path());
      plan.reclaimable_bytes += tree_size(it->path());
    }
  }
  return plan;
}

Status SysrootManager::apply_gc(const GcPlan& plan) const {
  for (const auto& path : plan.removable) {
    auto removed = fs::remove_tree(path, options_.store_root);
    if (!removed) {
      return fail(removed.error());
    }
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Safe extraction and local installation
// ---------------------------------------------------------------------------
Status extract_archive_safely(const fs::Path& archive, const fs::Path& dest_root,
                              const ExtractLimits& limits) {
  if (!fs::is_regular_file(archive)) {
    return fail(Error::make("cova::sysroot::archive_missing",
                            "archive " + fs::generic_string(archive) + " does not exist"));
  }
  auto created = fs::create_directories(dest_root);
  if (!created) {
    return created;
  }
  if (is_zip(archive)) {
    auto entries = list_zip(archive);
    if (!entries) {
      return fail(entries.error());
    }
    if (entries->size() > limits.max_entries) {
      return fail(Error::make("cova::sysroot::unsafe_archive", "archive has too many entries"));
    }
    auto extracted = extract_zip(archive, dest_root);
    if (!extracted) {
      return extracted;
    }
  } else {
    auto entries = list_tar(archive);
    if (!entries) {
      return fail(entries.error());
    }
    if (entries->size() > limits.max_entries) {
      return fail(Error::make("cova::sysroot::unsafe_archive", "archive has too many entries"));
    }
    auto extracted = extract_tar(archive, dest_root);
    if (!extracted) {
      return extracted;
    }
  }
  return validate_extracted_tree(dest_root, limits);
}

Status install_from_archive(const fs::Path& archive, const fs::Path& sysroots_root,
                            const SysrootManifest& manifest) {
  if (!fs::is_regular_file(archive)) {
    return fail(Error::make("cova::sysroot::archive_missing",
                            "archive " + fs::generic_string(archive) + " does not exist"));
  }
  auto created = fs::create_directories(sysroots_root);
  if (!created) {
    return created;
  }
  auto scratch = fs::make_temp_dir(sysroots_root, "cova-sysroot-install-");
  if (!scratch) {
    return fail(scratch.error());
  }
  auto extracted = extract_archive_safely(archive, *scratch);
  if (!extracted) {
    auto cleanup = fs::remove_tree(*scratch, sysroots_root);
    (void)cleanup;
    return extracted;
  }
  auto installed = install_tree(*scratch, manifest, sysroots_root, sysroots_root);
  auto cleanup = fs::remove_tree(*scratch, sysroots_root);
  (void)cleanup;
  if (!installed) {
    return fail(installed.error());
  }
  if (!installed->verified) {
    return fail(Error::make("cova::sysroot::verify_failed",
                            "the installed sysroot failed structural verification"));
  }
  return ok();
}

} // namespace cova::sysroot
