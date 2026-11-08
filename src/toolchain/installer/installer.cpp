// Cova — managed toolchain installation (PRD §7.2, §7.5, §8.6).
#include "toolchain/installer/installer.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/process/process.hpp"
#include "core/toml/toml.hpp"
#include "sysroot/manager/manager.hpp"
#include "toolchain/discovery/discovery.hpp"

namespace cova::toolchain {
namespace {

using toml::Table;
using toml::Value;

const Value* get(const Table& table, std::string_view key) {
  auto it = table.find(key);
  return it == table.end() ? nullptr : &it->second;
}

std::string read_string(const Table& table, std::string_view key) {
  const Value* value = get(table, key);
  return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

std::vector<std::string> read_array(const Table& table, std::string_view key) {
  const Value* value = get(table, key);
  return value == nullptr ? std::vector<std::string>{} : value->as_string_array();
}

bool is_remote_location(std::string_view location) {
  return str::starts_with(location, "http://") || str::starts_with(location, "https://") ||
         str::starts_with(location, "ftp://");
}

Result<std::vector<RegistryToolchainEntry>> load_entries(const stdfs::path& registry_root) {
  std::vector<RegistryToolchainEntry> entries;
  const auto index = registry_root / "index.toml";
  if (!fs::is_regular_file(index)) {
    return entries;
  }
  auto document = toml::parse_file(fs::generic_string(index));
  if (!document || !document->is_table()) {
    return fail<std::vector<RegistryToolchainEntry>>(
        Error::make("cova::registry::invalid", "registry index is not a valid TOML table")
            .with("path: " + fs::generic_string(index)));
  }
  const auto& table = *document->as_table();
  auto array = table.find("toolchain");
  if (array == table.end() || !array->second.is_array()) {
    return entries;
  }
  for (const auto& value : *array->second.as_array()) {
    const auto* item = value.as_table();
    if (item == nullptr) {
      continue;
    }
    RegistryToolchainEntry entry;
    entry.id = read_string(*item, "id");
    entry.name = read_string(*item, "name");
    entry.family = read_string(*item, "family");
    entry.version = read_string(*item, "version");
    entry.target = read_string(*item, "target");
    entry.archive = read_string(*item, "archive");
    entry.sha256 = str::to_lower(read_string(*item, "sha256"));
    entry.license = read_string(*item, "license");
    entry.source_url = read_string(*item, "source");
    entry.compiler = read_string(*item, "compiler");
    entry.provides = read_array(*item, "provides");
    if (entry.name.empty()) {
      entry.name = entry.id;
    }
    if (entry.id.empty()) {
      entry.id = entry.name;
    }
    if (auto size = get(*item, "size-bytes");
        size != nullptr && size->is_integer() && size->as_integer() > 0) {
      entry.size_bytes = static_cast<std::uint64_t>(size->as_integer());
    }
    if (entry.id.empty() || entry.family.empty() || entry.version.empty() ||
        entry.archive.empty() || entry.sha256.empty() || entry.compiler.empty()) {
      return fail<std::vector<RegistryToolchainEntry>>(
          Error::make("cova::registry::invalid", "registry toolchain entry `" + entry.id +
                                                     "` must declare id, family, version, "
                                                     "archive, sha256 and compiler"));
    }
    entries.push_back(std::move(entry));
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const RegistryToolchainEntry& a, const RegistryToolchainEntry& b) { return a.id < b.id; });
  return entries;
}

Result<stdfs::path> archive_path(const stdfs::path& registry_root, std::string_view archive) {
  const std::string text = str::trim(archive);
  if (is_remote_location(text)) {
    return fail<stdfs::path>(
        Error::make("cova::registry::download_unsupported",
                    "this build cannot download toolchain archives over the network")
            .with("archive: " + text)
            .with("place the archive in the local registry and reference it by "
                  "relative path"));
  }
  stdfs::path path(text);
  if (str::starts_with(text, "file://")) {
    path = stdfs::path(text.substr(7));
  }
  if (!path.is_absolute()) {
    const stdfs::path root = fs::absolute_normalized(registry_root);
    path = fs::absolute_normalized(root / path);
    if (!fs::is_within(path, root)) {
      return fail<stdfs::path>(
          Error::make("cova::registry::unsafe_path",
                      "toolchain archive path escapes the registry root: `" + text + "`"));
    }
  } else {
    path = fs::absolute_normalized(path);
  }
  if (!fs::is_regular_file(path)) {
    return fail<stdfs::path>(
        Error::make("cova::registry::archive_missing",
                    "toolchain archive does not exist: " + fs::generic_string(path)));
  }
  return path;
}

std::optional<stdfs::path> find_manifest_root(const stdfs::path& extracted) {
  for (const auto& file : fs::walk_files(extracted)) {
    if (file.filename() == "toolchain.toml") {
      return file.parent_path();
    }
  }
  // Archives commonly wrap everything in one directory.
  std::vector<stdfs::path> directories;
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(extracted, ec); it != stdfs::directory_iterator();
       it.increment(ec)) {
    if (it->is_directory()) {
      directories.push_back(it->path());
    }
  }
  if (directories.size() == 1) {
    return directories.front();
  }
  return std::nullopt;
}

Result<stdfs::path> locate_compiler(const stdfs::path& root, const RegistryToolchainEntry& entry) {
  if (!entry.compiler.empty()) {
    const auto direct = root / entry.compiler;
    if (fs::is_regular_file(direct) && proc::is_executable(direct)) {
      return direct;
    }
    // The compiler field may be relative to the archive root while the
    // toolchain manifest lives one level down (or vice versa).
    for (const auto& file : fs::walk_files(root)) {
      if (fs::generic_string(file.lexically_relative(root)) == entry.compiler &&
          proc::is_executable(file)) {
        return file;
      }
    }
  }
  for (const auto& file : fs::walk_files(root)) {
    if (proc::is_executable(file) && family_from_executable(file.filename().string())) {
      return file;
    }
  }
  return fail<stdfs::path>(Error::make(
      "cova::registry::invalid",
      "the toolchain archive does not contain the declared compiler `" + entry.compiler + "`"));
}

std::string manifest_text(const RegistryToolchainEntry& entry,
                          const ToolchainDescriptor& descriptor) {
  Table table;
  table.emplace("id", Value(entry.id));
  table.emplace("name", Value(entry.name));
  table.emplace("family", Value(descriptor.family));
  table.emplace("version", Value(descriptor.version.to_string()));
  if (!entry.target.empty()) {
    table.emplace("target", Value(entry.target));
  }
  table.emplace("compiler", Value(entry.compiler));
  table.emplace("sha256", Value(entry.sha256));
  if (!entry.license.empty()) {
    table.emplace("license", Value(entry.license));
  }
  if (!entry.source_url.empty()) {
    table.emplace("source", Value(entry.source_url));
  }
  table.emplace("host", Value(descriptor.host.to_string()));
  table.emplace("installed-by", Value("cova"));
  return toml::write(table);
}

bool version_matches_entry(const ToolchainDescriptor& descriptor,
                           const RegistryToolchainEntry& entry) {
  auto wanted = Version::parse(entry.version);
  if (!wanted) {
    return false;
  }
  return descriptor.version == *wanted;
}

Result<InstalledToolchain> verify_installed(const stdfs::path& root,
                                            const RegistryToolchainEntry& entry) {
  auto compiler = locate_compiler(root, entry);
  if (!compiler) {
    return fail<InstalledToolchain>(compiler.error());
  }
  auto descriptor = describe_compiler(*compiler, ToolchainSource::Managed);
  if (!descriptor) {
    return fail<InstalledToolchain>(descriptor.error());
  }
  if (descriptor->family != entry.family) {
    return fail<InstalledToolchain>(
        Error::make("cova::registry::verification_failed",
                    "installed compiler family does not match the registry entry")
            .with("expected: " + entry.family)
            .with("found: " + descriptor->family));
  }
  if (!version_matches_entry(*descriptor, entry)) {
    return fail<InstalledToolchain>(
        Error::make("cova::registry::verification_failed",
                    "installed compiler version does not match the registry entry")
            .with("expected: " + entry.version)
            .with("found: " + descriptor->version.to_string()));
  }
  if (!entry.target.empty()) {
    auto target = platform::Triple::parse(entry.target);
    if (target) {
      bool supported = false;
      for (const auto& text : descriptor->supported_targets) {
        if (text == target->to_string() || text == target->to_short_string()) {
          supported = true;
          break;
        }
      }
      if (!supported) {
        return fail<InstalledToolchain>(
            Error::make("cova::registry::verification_failed",
                        "installed compiler does not target " + target->to_string())
                .with("supported: " + str::join(descriptor->supported_targets, ", ")));
      }
    }
  }
  descriptor->source = ToolchainSource::Managed;
  descriptor->registry_id = entry.id;
  descriptor->trust_level = "verified";
  descriptor->content_hash = descriptor->compute_fingerprint();

  InstalledToolchain installed;
  installed.descriptor = std::move(*descriptor);
  installed.root = root;
  installed.entry = entry;
  return installed;
}

} // namespace

std::string RegistryToolchainEntry::to_string() const {
  std::string out = family + " " + version;
  if (!target.empty()) {
    out += " for " + target;
  }
  out += " (" + id + ")";
  return out;
}

ToolchainInstaller::ToolchainInstaller(ToolchainInstallOptions options)
    : options_(std::move(options)) {}

Result<std::vector<RegistryToolchainEntry>> ToolchainInstaller::available() const {
  return load_entries(options_.registry_root);
}

Result<InstalledToolchain> ToolchainInstaller::install(std::string_view id) {
  auto entries = load_entries(options_.registry_root);
  if (!entries) {
    return fail<InstalledToolchain>(entries.error());
  }
  const RegistryToolchainEntry* selected = nullptr;
  for (const auto& entry : *entries) {
    if (entry.id == id || entry.name == id) {
      selected = &entry;
      break;
    }
    const std::string family_version = entry.family + "-" + entry.version;
    if (family_version == id) {
      selected = &entry;
      break;
    }
  }
  if (selected == nullptr) {
    Error error =
        Error::make("cova::registry::not_found",
                    "no toolchain named `" + std::string(id) + "` is available in the registry")
            .with("registry: " + fs::generic_string(options_.registry_root));
    if (!entries->empty()) {
      std::string available;
      for (const auto& entry : *entries) {
        if (!available.empty()) {
          available += "; ";
        }
        available += entry.to_string();
      }
      error.with("available: " + available);
    }
    return fail<InstalledToolchain>(std::move(error));
  }

  const auto destination = options_.toolchains_root / selected->id;
  if (fs::is_directory(destination)) {
    auto existing = verify_installed(destination, *selected);
    if (existing) {
      return existing;
    }
    return fail<InstalledToolchain>(
        Error::make("cova::registry::verification_failed",
                    "a managed toolchain directory exists but does not match "
                    "its registry entry")
            .with("path: " + fs::generic_string(destination))
            .with(existing.error().message));
  }

  if (options_.offline) {
    return fail<InstalledToolchain>(
        Error::make("cova::registry::offline",
                    "offline mode forbids installing the toolchain `" + selected->id + "`"));
  }
  if (!options_.allow_install) {
    return fail<InstalledToolchain>(Error::make("cova::registry::install_disabled",
                                                "installation is disabled, so the toolchain `" +
                                                    selected->id + "` cannot be installed"));
  }

  auto source_archive = archive_path(options_.registry_root, selected->archive);
  if (!source_archive) {
    return fail<InstalledToolchain>(source_archive.error());
  }
  auto staged = sysroot::stage_verified_archive(options_.downloads_root, *source_archive,
                                                selected->sha256, selected->size_bytes);
  if (!staged) {
    return fail<InstalledToolchain>(staged.error());
  }

  auto created = fs::create_directories(options_.toolchains_root);
  if (!created) {
    return fail<InstalledToolchain>(created.error());
  }
  auto scratch = fs::make_temp_dir(options_.toolchains_root, "cova-toolchain-install-");
  if (!scratch) {
    return fail<InstalledToolchain>(scratch.error());
  }
  const auto extracted = *scratch / "extracted";
  auto extracted_status = fs::create_directories(extracted);
  if (!extracted_status) {
    auto cleanup = fs::remove_tree(*scratch, options_.toolchains_root);
    (void)cleanup;
    return fail<InstalledToolchain>(extracted_status.error());
  }
  auto extracted_ok = sysroot::extract_archive_safely(*staged, extracted);
  if (!extracted_ok) {
    auto cleanup = fs::remove_tree(*scratch, options_.toolchains_root);
    (void)cleanup;
    return fail<InstalledToolchain>(extracted_ok.error());
  }

  auto manifest_root = find_manifest_root(extracted);
  const stdfs::path package_root = manifest_root.value_or(extracted);
  auto checked = verify_installed(package_root, *selected);
  if (!checked) {
    auto cleanup = fs::remove_tree(*scratch, options_.toolchains_root);
    (void)cleanup;
    return fail<InstalledToolchain>(checked.error());
  }

  auto renamed = fs::rename_path(package_root, destination);
  if (!renamed) {
    auto cleanup = fs::remove_tree(*scratch, options_.toolchains_root);
    (void)cleanup;
    return fail<InstalledToolchain>(renamed.error());
  }
  auto cleanup = fs::remove_tree(*scratch, options_.toolchains_root);
  (void)cleanup;

  auto installed = verify_installed(destination, *selected);
  if (!installed) {
    auto removed = fs::remove_tree(destination, options_.toolchains_root);
    (void)removed;
    return fail<InstalledToolchain>(installed.error());
  }
  auto manifest_written = fs::write_file_atomic(destination / "toolchain.toml",
                                                manifest_text(*selected, installed->descriptor));
  if (!manifest_written) {
    auto removed = fs::remove_tree(destination, options_.toolchains_root);
    (void)removed;
    return fail<InstalledToolchain>(manifest_written.error());
  }
  return installed;
}

Result<InstalledToolchain> ToolchainInstaller::verify(std::string_view id) const {
  auto entries = load_entries(options_.registry_root);
  if (!entries) {
    return fail<InstalledToolchain>(entries.error());
  }
  for (const auto& entry : *entries) {
    if (entry.id != id && entry.name != id) {
      continue;
    }
    const auto root = options_.toolchains_root / entry.id;
    if (!fs::is_directory(root)) {
      return fail<InstalledToolchain>(Error::make(
          "cova::registry::not_installed",
          "toolchain `" + entry.id + "` is not installed at " + fs::generic_string(root)));
    }
    return verify_installed(root, entry);
  }
  return fail<InstalledToolchain>(
      Error::make("cova::registry::not_found",
                  "no toolchain named `" + std::string(id) + "` is present in the registry"));
}

} // namespace cova::toolchain
