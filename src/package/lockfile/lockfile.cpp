// Cova — lockfiles (PRD §15).
#include "package/lockfile/lockfile.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"

namespace cova::lockfile {
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
  if (value == nullptr) {
    return {};
  }
  return value->as_string_array();
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

Value table_value(Table table) { return Value(std::move(table)); }

Table package_table(const LockedPackage& package) {
  Table table;
  table.emplace("name", Value(package.name));
  table.emplace("version", Value(package.version));
  if (package.source_url) {
    table.emplace("source-url", Value(*package.source_url));
  }
  if (package.git_rev) {
    table.emplace("git-rev", Value(*package.git_rev));
  }
  if (package.registry) {
    table.emplace("registry", Value(*package.registry));
  }
  if (package.path) {
    table.emplace("path", Value(*package.path));
  }
  if (!package.source_hash.empty()) {
    table.emplace("source-hash", Value(package.source_hash));
  }
  put_array(table, "checksum", package.checksum);
  put_array(table, "features", package.features);
  put_array(table, "dependencies", package.dependencies);
  if (package.dev) {
    table.emplace("dev", Value(true));
  }
  return table;
}

Table toolchain_table(const LockedToolchain& entry) {
  Table table;
  table.emplace("target", Value(entry.target));
  table.emplace("toolchain-id", Value(entry.toolchain_id));
  if (!entry.toolchain_family.empty()) {
    table.emplace("toolchain-family", Value(entry.toolchain_family));
  }
  if (!entry.toolchain_version.empty()) {
    table.emplace("toolchain-version", Value(entry.toolchain_version));
  }
  if (!entry.content_hash.empty()) {
    table.emplace("content-hash", Value(entry.content_hash));
  }
  if (!entry.source.empty()) {
    table.emplace("source", Value(entry.source));
  }
  if (!entry.host.empty()) {
    table.emplace("host", Value(entry.host));
  }
  if (!entry.system_fingerprint.empty()) {
    table.emplace("system-fingerprint", Value(entry.system_fingerprint));
  }
  if (!entry.sysroot_id.empty()) {
    table.emplace("sysroot-id", Value(entry.sysroot_id));
  }
  if (!entry.sysroot_hash.empty()) {
    table.emplace("sysroot-hash", Value(entry.sysroot_hash));
  }
  if (!entry.abi_key.empty()) {
    table.emplace("abi-key", Value(entry.abi_key));
  }
  put_array(table, "components", entry.components);
  return table;
}

}  // namespace

// ---------------------------------------------------------------------------
// Dependency lock
// ---------------------------------------------------------------------------
std::string LockedPackage::identity() const {
  std::string out = name + "@" + version;
  if (source_url) {
    out += " (" + *source_url + ")";
  } else if (path) {
    out += " (path " + *path + ")";
  } else if (registry) {
    out += " (registry " + *registry + ")";
  }
  return out;
}

std::string LockedPackage::to_toml() const { return toml::write(package_table(*this)); }

const LockedPackage* DependencyLock::find(std::string_view name) const {
  for (const auto& package : packages) {
    if (package.name == name) {
      return &package;
    }
  }
  return nullptr;
}

std::string DependencyLock::to_toml() const {
  Table top;
  top.emplace("version", Value(version));
  if (!generated_by.empty()) {
    top.emplace("generated-by", Value(generated_by));
  }
  if (!manifest_fingerprint.empty()) {
    top.emplace("manifest-fingerprint", Value(manifest_fingerprint));
  }
  if (!packages.empty()) {
    toml::Array array;
    std::vector<const LockedPackage*> sorted;
    sorted.reserve(packages.size());
    for (const auto& package : packages) {
      sorted.push_back(&package);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const LockedPackage* a, const LockedPackage* b) { return a->name < b->name; });
    for (const auto* package : sorted) {
      array.emplace_back(table_value(package_table(*package)));
    }
    top.emplace("package", Value(std::move(array)));
  }
  return toml::write(top);
}

Result<DependencyLock> DependencyLock::from_toml(std::string_view text) {
  auto document = toml::parse(text, "cova.lock");
  if (!document) {
    return fail<DependencyLock>(document.error());
  }
  if (!document->is_table()) {
    return fail<DependencyLock>(
        Error::make("cova::lock::structure", "`cova.lock` must contain a TOML table"));
  }
  const Table& top = *document->as_table();
  DependencyLock lock;
  lock.version = string_or(top, "version", "1");
  lock.generated_by = string_or(top, "generated-by");
  lock.manifest_fingerprint = string_or(top, "manifest-fingerprint");
  if (const Value* packages = get(top, "package")) {
    if (!packages->is_array()) {
      return fail<DependencyLock>(
          Error::make("cova::lock::structure", "`package` in cova.lock must be an array of tables"));
    }
    for (const auto& entry : *packages->as_array()) {
      if (!entry.is_table()) {
        return fail<DependencyLock>(
            Error::make("cova::lock::structure", "each `package` entry must be a table"));
      }
      const Table& table = *entry.as_table();
      LockedPackage package;
      package.name = string_or(table, "name");
      package.version = string_or(table, "version");
      if (package.name.empty()) {
        return fail<DependencyLock>(
            Error::make("cova::lock::structure", "a locked package is missing `name`"));
      }
      if (auto value = get(table, "source-url"); value != nullptr && value->is_string()) {
        package.source_url = value->as_string();
      }
      if (auto value = get(table, "git-rev"); value != nullptr && value->is_string()) {
        package.git_rev = value->as_string();
      }
      if (auto value = get(table, "registry"); value != nullptr && value->is_string()) {
        package.registry = value->as_string();
      }
      if (auto value = get(table, "path"); value != nullptr && value->is_string()) {
        package.path = value->as_string();
      }
      package.source_hash = string_or(table, "source-hash");
      package.checksum = array_or(table, "checksum");
      package.features = array_or(table, "features");
      package.dependencies = array_or(table, "dependencies");
      if (auto value = get(table, "dev"); value != nullptr && value->is_bool()) {
        package.dev = value->as_bool();
      }
      lock.packages.push_back(std::move(package));
    }
  }
  return lock;
}

Result<DependencyLock> DependencyLock::load(const stdfs::path& path) {
  auto text = fs::read_file(path);
  if (!text) {
    return fail<DependencyLock>(text.error());
  }
  return from_toml(*text);
}

Status DependencyLock::save(const stdfs::path& path) const {
  if (path.has_parent_path()) {
    auto status = fs::create_directories(path.parent_path());
    if (!status) {
      return status;
    }
  }
  return fs::write_file_atomic(path, to_toml());
}

// ---------------------------------------------------------------------------
// Toolchain lock
// ---------------------------------------------------------------------------
std::string LockedToolchain::to_toml() const { return toml::write(toolchain_table(*this)); }

const LockedToolchain* ToolchainLock::for_target(std::string_view target) const {
  for (const auto& entry : entries) {
    if (entry.target == target) {
      return &entry;
    }
  }
  return nullptr;
}

std::string ToolchainLock::to_toml() const {
  Table top;
  top.emplace("version", Value(version));
  if (!generated_by.empty()) {
    top.emplace("generated-by", Value(generated_by));
  }
  if (!entries.empty()) {
    toml::Array array;
    std::vector<const LockedToolchain*> sorted;
    sorted.reserve(entries.size());
    for (const auto& entry : entries) {
      sorted.push_back(&entry);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const LockedToolchain* a, const LockedToolchain* b) {
                return a->target < b->target;
              });
    for (const auto* entry : sorted) {
      array.emplace_back(table_value(toolchain_table(*entry)));
    }
    top.emplace("toolchain", Value(std::move(array)));
  }
  return toml::write(top);
}

Result<ToolchainLock> ToolchainLock::from_toml(std::string_view text) {
  auto document = toml::parse(text, "cova-toolchain.lock");
  if (!document) {
    return fail<ToolchainLock>(document.error());
  }
  if (!document->is_table()) {
    return fail<ToolchainLock>(Error::make(
        "cova::lock::structure", "`cova-toolchain.lock` must contain a TOML table"));
  }
  const Table& top = *document->as_table();
  ToolchainLock lock;
  lock.version = string_or(top, "version", "1");
  lock.generated_by = string_or(top, "generated-by");
  if (const Value* entries = get(top, "toolchain")) {
    if (!entries->is_array()) {
      return fail<ToolchainLock>(Error::make(
          "cova::lock::structure", "`toolchain` in cova-toolchain.lock must be an array"));
    }
    for (const auto& value : *entries->as_array()) {
      if (!value.is_table()) {
        return fail<ToolchainLock>(
            Error::make("cova::lock::structure", "each toolchain lock entry must be a table"));
      }
      const Table& table = *value.as_table();
      LockedToolchain entry;
      entry.target = string_or(table, "target");
      entry.toolchain_id = string_or(table, "toolchain-id");
      entry.toolchain_family = string_or(table, "toolchain-family");
      entry.toolchain_version = string_or(table, "toolchain-version");
      entry.content_hash = string_or(table, "content-hash");
      entry.source = string_or(table, "source");
      entry.host = string_or(table, "host");
      entry.system_fingerprint = string_or(table, "system-fingerprint");
      entry.sysroot_id = string_or(table, "sysroot-id");
      entry.sysroot_hash = string_or(table, "sysroot-hash");
      entry.abi_key = string_or(table, "abi-key");
      entry.components = array_or(table, "components");
      if (entry.target.empty() || entry.toolchain_id.empty()) {
        return fail<ToolchainLock>(Error::make(
            "cova::lock::structure", "a toolchain lock entry is missing `target` or `toolchain-id`"));
      }
      lock.entries.push_back(std::move(entry));
    }
  }
  return lock;
}

Result<ToolchainLock> ToolchainLock::load(const stdfs::path& path) {
  auto text = fs::read_file(path);
  if (!text) {
    return fail<ToolchainLock>(text.error());
  }
  return from_toml(*text);
}

Status ToolchainLock::save(const stdfs::path& path) const {
  if (path.has_parent_path()) {
    auto status = fs::create_directories(path.parent_path());
    if (!status) {
      return status;
    }
  }
  return fs::write_file_atomic(path, to_toml());
}

// ---------------------------------------------------------------------------
// Conversion and drift detection
// ---------------------------------------------------------------------------
LockedToolchain lock_entry_from(const toolchain::ResolvedToolchain& resolved) {
  LockedToolchain entry;
  entry.target = resolved.target.to_string();
  entry.toolchain_id = resolved.descriptor.id;
  entry.toolchain_family = resolved.descriptor.family;
  entry.toolchain_version = resolved.descriptor.version.to_string();
  entry.content_hash = resolved.descriptor.content_hash;
  entry.source = std::string(toolchain::to_string(resolved.descriptor.source));
  entry.host = resolved.descriptor.host.to_string();
  if (resolved.descriptor.source == toolchain::ToolchainSource::System) {
    entry.system_fingerprint = resolved.descriptor.compute_fingerprint();
    if (entry.content_hash.empty()) {
      entry.content_hash = entry.system_fingerprint;
    }
  }
  entry.sysroot_id = resolved.sysroot_id;
  entry.sysroot_hash = resolved.sysroot_fingerprint.empty() ? resolved.sysroot_id
                                                             : resolved.sysroot_fingerprint;
  entry.abi_key = resolved.abi_signature.fingerprint_key();
  for (const auto& component : resolved.descriptor.components) {
    entry.components.push_back(component.role + "=" + fs::generic_string(component.path));
  }
  std::sort(entry.components.begin(), entry.components.end());
  return entry;
}

LockMismatch check_against_lock(const toolchain::ResolvedToolchain& resolved,
                                const LockedToolchain& locked) {
  LockMismatch mismatch;
  if (locked.target != resolved.target.to_string()) {
    mismatch.differences.push_back("target changed: " + locked.target + " -> " +
                                   resolved.target.to_string());
  }
  if (locked.toolchain_id != resolved.descriptor.id) {
    mismatch.differences.push_back("toolchain identity changed: " + locked.toolchain_id + " -> " +
                                   resolved.descriptor.id);
  }
  if (!locked.content_hash.empty() && !resolved.descriptor.content_hash.empty() &&
      locked.content_hash != resolved.descriptor.content_hash) {
    mismatch.differences.push_back("toolchain content hash changed");
  }
  if (locked.sysroot_id != resolved.sysroot_id) {
    mismatch.differences.push_back("sysroot changed: " + locked.sysroot_id + " -> " +
                                   resolved.sysroot_id);
  }
  const std::string sysroot_hash = resolved.sysroot_fingerprint.empty()
                                       ? resolved.sysroot_id
                                       : resolved.sysroot_fingerprint;
  if (!locked.sysroot_hash.empty() && !sysroot_hash.empty() &&
      locked.sysroot_hash != sysroot_hash) {
    mismatch.differences.push_back("sysroot content changed");
  }
  const std::string abi_key = resolved.abi_signature.fingerprint_key();
  if (!locked.abi_key.empty() && locked.abi_key != abi_key) {
    mismatch.differences.push_back("ABI signature changed");
  }
  mismatch.matches = mismatch.differences.empty();
  return mismatch;
}

bool needs_rewrite(const ToolchainLock& lock, const toolchain::ResolvedToolchain& resolved,
                   bool locked_mode) {
  if (locked_mode) {
    return false;
  }
  const LockedToolchain* existing = lock.for_target(resolved.target.to_string());
  if (existing == nullptr) {
    return true;
  }
  LockedToolchain desired = lock_entry_from(resolved);
  if (existing->toolchain_id != desired.toolchain_id) {
    return true;
  }
  if (!desired.content_hash.empty() && existing->content_hash != desired.content_hash) {
    return true;
  }
  if (existing->sysroot_id != desired.sysroot_id) {
    return true;
  }
  if (!desired.sysroot_hash.empty() && existing->sysroot_hash != desired.sysroot_hash) {
    return true;
  }
  if (existing->abi_key != desired.abi_key) {
    return true;
  }
  return false;
}

std::string manifest_fingerprint(std::string_view manifest_text,
                                 const std::vector<std::string>& member_fingerprints) {
  Fingerprint fingerprint;
  fingerprint.add(manifest_text);
  std::vector<std::string> sorted = member_fingerprints;
  std::sort(sorted.begin(), sorted.end());
  for (const auto& member : sorted) {
    fingerprint.add(member);
  }
  return fingerprint.finish_hex();
}

}  // namespace cova::lockfile
