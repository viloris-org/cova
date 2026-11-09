// Cova — incremental build state and artifact cache.
#include "build/cache/cache.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/toml/toml.hpp"

namespace cova::build {
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

std::vector<std::string> strings_or(const Table& table, std::string_view key) {
  const Value* value = get(table, key);
  return value == nullptr ? std::vector<std::string>{} : value->as_string_array();
}

void put_strings(Table& table, std::string_view key, const std::vector<std::string>& values) {
  if (values.empty()) {
    return;
  }
  toml::Array array;
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  table.emplace(std::string(key), Value(std::move(array)));
}


}  // namespace

CacheSettings default_cache_settings() {
  CacheSettings settings;
  auto dirs = fs::cova_dirs();
  if (dirs) {
    settings.root = dirs->cache / "build";
  } else {
    settings.root = stdfs::temp_directory_path() / "cova-cache";
  }
  return settings;
}

// ---------------------------------------------------------------------------
// Action state
// ---------------------------------------------------------------------------
Result<BuildState> BuildState::load(const Path& path) {
  BuildState state;
  if (!fs::is_regular_file(path)) {
    return state;
  }
  auto text = fs::read_file(path);
  if (!text) {
    return fail<BuildState>(text.error());
  }
  auto document = toml::parse(*text, fs::generic_string(path));
  if (!document || !document->is_table()) {
    // A corrupt incremental state is not fatal: rebuilding is always safe.
    return state;
  }
  const Table& top = *document->as_table();
  if (string_or(top, "schema-version") != kBuildStateSchemaVersion) {
    // State written under different rules is not interpretable safely.
    return state;
  }
  if (const Value* records = get(top, "record"); records != nullptr && records->is_array()) {
    for (const auto& entry : *records->as_array()) {
      if (!entry.is_table()) {
        continue;
      }
      const Table& table = *entry.as_table();
      ActionRecord record;
      record.cache_key = string_or(table, "cache-key");
      record.output_paths = strings_or(table, "output-paths");
      record.input_paths = strings_or(table, "input-paths");
      record.toolchain_fingerprint = string_or(table, "toolchain-fingerprint");
      record.sysroot_fingerprint = string_or(table, "sysroot-fingerprint");
      if (auto value = get(table, "completed-at-ns"); value != nullptr && value->is_integer()) {
        record.completed_at_ns = static_cast<std::uint64_t>(value->as_integer());
      }
      if (auto value = get(table, "duration-ms"); value != nullptr && value->is_integer()) {
        record.duration_ms = static_cast<std::uint32_t>(value->as_integer());
      }
      if (auto value = get(table, "succeeded"); value != nullptr && value->is_bool()) {
        record.succeeded = value->as_bool();
      }
      // Hashes are stored as "path\t digest" strings to keep the TOML simple.
      for (const auto& encoded : strings_or(table, "input-hashes")) {
        const auto position = encoded.rfind('\t');
        if (position == std::string::npos) {
          continue;
        }
        record.input_hashes.emplace_back(encoded.substr(0, position),
                                         encoded.substr(position + 1));
      }
      state.records_.push_back(std::move(record));
    }
  }
  return state;
}

Status BuildState::save(const Path& path) const {
  if (path.has_parent_path()) {
    auto created = fs::create_directories(path.parent_path());
    if (!created) {
      return created;
    }
  }
  Table top;
  toml::Array records;
  for (const auto& record : records_) {
    Table table;
    table.emplace("cache-key", Value(record.cache_key));
    put_strings(table, "output-paths", record.output_paths);
    put_strings(table, "input-paths", record.input_paths);
    std::vector<std::string> hashes;
    hashes.reserve(record.input_hashes.size());
    for (const auto& [input, digest] : record.input_hashes) {
      hashes.push_back(input + "\t" + digest);
    }
    put_strings(table, "input-hashes", hashes);
    if (!record.toolchain_fingerprint.empty()) {
      table.emplace("toolchain-fingerprint", Value(record.toolchain_fingerprint));
    }
    if (!record.sysroot_fingerprint.empty()) {
      table.emplace("sysroot-fingerprint", Value(record.sysroot_fingerprint));
    }
    table.emplace("completed-at-ns",
                  Value(static_cast<std::int64_t>(record.completed_at_ns)));
    table.emplace("duration-ms", Value(static_cast<std::int64_t>(record.duration_ms)));
    table.emplace("succeeded", Value(record.succeeded));
    records.emplace_back(Value(std::move(table)));
  }
  top.emplace("schema-version", Value(std::string(kBuildStateSchemaVersion)));
  top.emplace("record", Value(std::move(records)));
  return fs::write_file_atomic(path, toml::write(top));
}

bool BuildState::is_up_to_date(const Action& action, std::string& reason) const {
  if (action.computed_key.empty()) {
    // Output-path matching without an action identity is not a correctness
    // proof. Callers must compute the key first.
    reason = "no action identity";
    return false;
  }
  const ActionRecord* match = nullptr;
  for (const auto& record : records_) {
    if (record.cache_key == action.computed_key) {
      match = &record;
      break;
    }
  }
  if (match == nullptr) {
    reason = "no previous build record";
    return false;
  }
  if (!match->succeeded) {
    reason = "the previous build failed";
    return false;
  }
  for (const auto& output : match->output_paths) {
    if (!fs::exists(output)) {
      reason = "output is missing: " + output;
      return false;
    }
  }
  FileHasher hasher;
  for (const auto& [input, expected] : match->input_hashes) {
    auto digest = hasher.hash_file(input);
    if (!digest) {
      reason = "input is missing: " + input;
      return false;
    }
    if (cova::to_hex(*digest) != expected) {
      reason = "input changed: " + input;
      return false;
    }
  }
  reason = "up to date";
  return true;
}

void BuildState::record(ActionRecord record) {
  if (!record.cache_key.empty()) {
    for (auto& existing : records_) {
      if (existing.cache_key == record.cache_key) {
        existing = std::move(record);
        return;
      }
    }
  }
  for (auto& existing : records_) {
    if (existing.output_paths == record.output_paths) {
      existing = std::move(record);
      return;
    }
  }
  records_.push_back(std::move(record));
}

// ---------------------------------------------------------------------------
// Depfiles
// ---------------------------------------------------------------------------
Result<Depfile> Depfile::parse(std::string_view text, const Path& base) {
  Depfile depfile;
  std::string joined;
  for (const auto& line : str::split(text, '\n')) {
    if (!line.empty() && line.back() == '\\') {
      joined.append(line.substr(0, line.size() - 1));
      joined.push_back(' ');
    } else {
      joined.append(line);
      joined.push_back(' ');
    }
  }
  auto colon = joined.find(':');
  if (colon == std::string::npos) {
    return fail<Depfile>(Error::make("cova::build::depfile", "dependency file has no target line"));
  }
  const std::string dependencies = joined.substr(colon + 1);
  std::string current;
  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    const char character = dependencies[index];
    if (character == '\\' && index + 1 < dependencies.size()) {
      current.push_back(dependencies[++index]);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character))) {
      if (!current.empty()) {
        depfile.dependencies.push_back(base / current);
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }
  if (!current.empty()) {
    depfile.dependencies.push_back(base / current);
  }
  return depfile;
}

Result<Depfile> Depfile::load(const Path& path, const Path& base) {
  auto text = fs::read_file(path);
  if (!text) {
    return fail<Depfile>(text.error());
  }
  return parse(*text, base);
}

// ---------------------------------------------------------------------------
// File hashing and artifact cache
// ---------------------------------------------------------------------------
Result<Digest> FileHasher::hash_file(const Path& path) const {
  return Sha256::of_file(path);
}

namespace {

std::atomic<std::uint64_t> temporary_counter{0};

struct Artifact {
  std::string path;
  std::string blob;
  std::string sha256;
  std::uint64_t size = 0;
};

std::string temporary_suffix() {
  const auto count = temporary_counter.fetch_add(1);
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(static_cast<unsigned long long>(ticks)) + "-" +
         std::to_string(static_cast<unsigned long long>(count));
}

Result<std::vector<Artifact>> compute_artifacts(const Action& action) {
  std::vector<Artifact> artifacts;
  artifacts.reserve(action.outputs.size());
  for (const auto& output : action.outputs) {
    if (!fs::is_regular_file(output.path)) {
      return fail<std::vector<Artifact>>(
          Error::make("cova::build::cache_store",
                      "cannot cache a missing output: " + fs::generic_string(output.path)));
    }
    auto digest = Sha256::hex_of_file(output.path);
    if (!digest) {
      return fail<std::vector<Artifact>>(digest.error());
    }
    Artifact artifact;
    artifact.path = fs::generic_string(output.path);
    artifact.sha256 = *digest;
    // Content-addressed blob names make concurrent writers that agree on the
    // content idempotent, and never overwrite a blob with different bytes.
    artifact.blob = "blob-" + *digest;
    artifact.size = fs::file_size(output.path);
    artifacts.push_back(std::move(artifact));
  }
  return artifacts;
}

// Returns the existing manifest's artifacts when it is complete (manifest and
// every blob present and matching). A corrupt entry returns nullopt, which the
// caller treats as safe to replace.
std::optional<std::vector<Artifact>> read_complete_entry(const Path& directory) {
  const auto manifest_path = directory / "manifest.toml";
  if (!fs::is_regular_file(manifest_path)) {
    return std::nullopt;
  }
  auto text = fs::read_file(manifest_path);
  if (!text) {
    return std::nullopt;
  }
  auto document = toml::parse(*text, fs::generic_string(manifest_path));
  if (!document || !document->is_table()) {
    return std::nullopt;
  }
  const Table& top = *document->as_table();
  if (string_or(top, "schema-version") != kArtifactCacheSchemaVersion) {
    return std::nullopt;
  }
  const Value* outputs = get(top, "output");
  if (outputs == nullptr || !outputs->is_array()) {
    return std::nullopt;
  }
  std::vector<Artifact> artifacts;
  for (const auto& entry : *outputs->as_array()) {
    if (!entry.is_table()) {
      return std::nullopt;
    }
    const Table& table = *entry.as_table();
    Artifact artifact;
    artifact.path = string_or(table, "path");
    artifact.blob = string_or(table, "blob");
    artifact.sha256 = string_or(table, "sha256");
    if (artifact.path.empty() || artifact.blob.empty() || artifact.sha256.empty()) {
      return std::nullopt;
    }
    if (!fs::is_regular_file(directory / artifact.blob)) {
      return std::nullopt;
    }
    auto digest = Sha256::hex_of_file(directory / artifact.blob);
    if (!digest || *digest != artifact.sha256) {
      return std::nullopt;
    }
    artifacts.push_back(std::move(artifact));
  }
  return artifacts;
}

bool same_artifacts(const std::vector<Artifact>& left, const std::vector<Artifact>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  auto sorted_left = left;
  auto sorted_right = right;
  const auto by_path = [](const Artifact& a, const Artifact& b) { return a.path < b.path; };
  std::sort(sorted_left.begin(), sorted_left.end(), by_path);
  std::sort(sorted_right.begin(), sorted_right.end(), by_path);
  for (std::size_t index = 0; index < sorted_left.size(); ++index) {
    if (sorted_left[index].path != sorted_right[index].path ||
        sorted_left[index].sha256 != sorted_right[index].sha256) {
      return false;
    }
  }
  return true;
}

Table artifact_manifest(const std::string& key, const std::vector<Artifact>& artifacts) {
  Table top;
  top.emplace("schema-version", Value(std::string(kArtifactCacheSchemaVersion)));
  top.emplace("key", Value(key));
  toml::Array outputs;
  for (const auto& artifact : artifacts) {
    Table entry;
    entry.emplace("path", Value(artifact.path));
    entry.emplace("blob", Value(artifact.blob));
    entry.emplace("sha256", Value(artifact.sha256));
    entry.emplace("size", Value(static_cast<std::int64_t>(artifact.size)));
    outputs.emplace_back(Value(std::move(entry)));
  }
  top.emplace("output", Value(std::move(outputs)));
  return top;
}

Status record_nondeterminism(const Path& directory, const std::string& key,
                             const std::vector<Artifact>& expected,
                             const std::vector<Artifact>& existing) {
  Table top;
  top.emplace("schema-version", Value(std::string(kArtifactCacheSchemaVersion)));
  top.emplace("key", Value(key));
  top.emplace("kind", Value(std::string("artifact-content-mismatch")));
  auto encode = [](const std::vector<Artifact>& artifacts) {
    toml::Array array;
    for (const auto& artifact : artifacts) {
      Table entry;
      entry.emplace("path", Value(artifact.path));
      entry.emplace("sha256", Value(artifact.sha256));
      array.emplace_back(Value(std::move(entry)));
    }
    return Value(std::move(array));
  };
  top.emplace("expected", encode(expected));
  top.emplace("existing", encode(existing));
  return fs::write_file_atomic(directory / "nondeterminism.toml", toml::write(top));
}

}  // namespace

Status cache_store(const CacheSettings& settings, const Action& action,
                   const std::string& key) {
  if (!settings.enabled || settings.read_only || action.outputs.empty() || key.empty()) {
    return ok();
  }
  auto artifacts = compute_artifacts(action);
  if (!artifacts) {
    return fail(artifacts.error());
  }
  const std::string prefix = key.size() >= 2 ? key.substr(0, 2) : "00";
  const Path actions_root = settings.root / "actions";
  const Path prefix_dir = actions_root / prefix;
  auto prepared = fs::create_directories(prefix_dir);
  if (!prepared) {
    return prepared;
  }
  const Path directory = prefix_dir / key;

  // `create_directory` is the atomic ownership token for this key. Only the
  // owner writes blobs and the manifest; a reader sees either no manifest or a
  // complete one.
  std::error_code ec;
  bool owns = stdfs::create_directory(directory, ec);
  if (!owns && !ec) {
    const Path manifest_path = directory / "manifest.toml";
    for (int attempt = 0; attempt < 5 && !owns; ++attempt) {
      if (auto existing = read_complete_entry(directory)) {
        if (same_artifacts(*existing, *artifacts)) {
          return ok();
        }
        auto recorded = record_nondeterminism(directory, key, *artifacts, *existing);
        (void)recorded;
        return fail(Error::make(
                        "cova::build::cache_nondeterminism",
                        "two executions with the same action identity produced different artifacts")
                        .with("key: " + key)
                        .with("the existing entry was left untouched; shared reuse is suspended")
                        .with("conflict record: " +
                              fs::generic_string(directory / "nondeterminism.toml")));
      }
      if (fs::is_regular_file(manifest_path)) {
        // A manifest that cannot be validated is corruption, not a live
        // writer. Replace it; a reader only ever sees a miss.
        auto removed = fs::remove_tree(directory, actions_root);
        if (!removed) {
          return fail(removed.error());
        }
        owns = stdfs::create_directory(directory, ec);
        break;
      }
      // No manifest yet: another writer owns the directory, or a previous
      // writer crashed. A stale directory is replaced; a live one is waited
      // for briefly.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!owns && !ec) {
      const auto mtime = fs::mtime_ns(directory);
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      const bool stale = !mtime || now - *mtime > 300'000'000'000LL;
      if (!stale) {
        return fail(Error::make("cova::build::cache_busy",
                                "another process is publishing this cache entry"));
      }
      auto removed = fs::remove_tree(directory, actions_root);
      (void)removed;
      owns = stdfs::create_directory(directory, ec);
    }
  }
  if (!owns) {
    return fail(Error::make("cova::build::cache_store",
                            "cannot create the cache entry directory")
                    .with("path: " + fs::generic_string(directory))
                    .with("reason: " + ec.message()));
  }

  bool completed = false;
  auto cleanup = [&]() {
    if (!completed) {
      auto removed = fs::remove_tree(directory, actions_root);
      (void)removed;
    }
  };

  for (const auto& artifact : *artifacts) {
    const Path destination = directory / artifact.blob;
    if (fs::is_regular_file(destination)) {
      auto digest = Sha256::hex_of_file(destination);
      if (digest && *digest == artifact.sha256) {
        continue;
      }
    }
    auto copied = fs::copy_file(Path(artifact.path), destination, true);
    if (!copied) {
      cleanup();
      return copied;
    }
    auto digest = Sha256::hex_of_file(destination);
    if (!digest || *digest != artifact.sha256) {
      cleanup();
      return fail(Error::make("cova::build::cache_store",
                              "an artifact changed while it was being published")
                      .with("path: " + artifact.path));
    }
  }

  auto manifest = fs::write_file_atomic(directory / "manifest.toml",
                                        toml::write(artifact_manifest(key, *artifacts)));
  if (!manifest) {
    cleanup();
    return manifest;
  }
  completed = true;
  return ok();
}

Result<bool> cache_restore(const CacheSettings& settings, const Action& action,
                           const std::string& key) {
  if (!settings.enabled || action.outputs.empty() || key.empty()) {
    return false;
  }
  const std::string prefix = key.size() >= 2 ? key.substr(0, 2) : "00";
  const Path directory = settings.root / "actions" / prefix / key;
  if (fs::is_regular_file(directory / "nondeterminism.toml")) {
    // The key produced different content in two executions. Reuse stays
    // suspended until the incident is explained and the entry is removed.
    return false;
  }
  const auto manifest_path = directory / "manifest.toml";
  if (!fs::is_regular_file(manifest_path)) {
    return false;
  }
  auto text = fs::read_file(manifest_path);
  if (!text) {
    return false;
  }
  auto document = toml::parse(*text, fs::generic_string(manifest_path));
  if (!document || !document->is_table()) {
    return false;
  }
  const Table& top = *document->as_table();
  if (string_or(top, "schema-version") != kArtifactCacheSchemaVersion ||
      string_or(top, "key") != key) {
    return false;
  }
  const Value* outputs = get(top, "output");
  if (outputs == nullptr || !outputs->is_array()) {
    return false;
  }
  // The entry must describe exactly the outputs this action promises.
  std::vector<Artifact> artifacts;
  for (const auto& entry : *outputs->as_array()) {
    if (!entry.is_table()) {
      return false;
    }
    const Table& table = *entry.as_table();
    Artifact artifact;
    artifact.path = string_or(table, "path");
    artifact.blob = string_or(table, "blob");
    artifact.sha256 = string_or(table, "sha256");
    if (artifact.path.empty() || artifact.blob.empty() || artifact.sha256.empty()) {
      return false;
    }
    artifacts.push_back(std::move(artifact));
  }
  if (artifacts.size() != action.outputs.size()) {
    return false;
  }
  std::vector<std::string> expected_paths;
  for (const auto& output : action.outputs) {
    expected_paths.push_back(fs::generic_string(output.path));
  }
  std::vector<std::string> recorded_paths;
  for (const auto& artifact : artifacts) {
    recorded_paths.push_back(artifact.path);
  }
  std::sort(expected_paths.begin(), expected_paths.end());
  std::sort(recorded_paths.begin(), recorded_paths.end());
  if (expected_paths != recorded_paths) {
    return false;
  }

  // Validate every blob before touching any destination.
  for (const auto& artifact : artifacts) {
    const auto from = directory / artifact.blob;
    if (!fs::is_regular_file(from)) {
      return false;
    }
    auto digest = Sha256::hex_of_file(from);
    if (!digest || *digest != artifact.sha256) {
      return false;
    }
  }

  // Stage every output beside its destination, then rename. This keeps a
  // partially copied file from ever being observed as a valid artifact.
  struct Staged {
    Path source;
    Path destination;
    std::vector<Path> temporaries;
  };
  std::vector<Staged> staged;
  auto fail_cleanup = [&](Error error) -> Result<bool> {
    for (const auto& entry : staged) {
      for (const auto& temporary : entry.temporaries) {
        std::error_code ignored;
        stdfs::remove(temporary, ignored);
      }
    }
    return fail<bool>(std::move(error));
  };
  for (const auto& artifact : artifacts) {
    const Path destination(artifact.path);
    if (destination.has_parent_path()) {
      auto created = fs::create_directories(destination.parent_path());
      if (!created) {
        return fail_cleanup(created.error());
      }
    }
    const Path temporary =
        Path(artifact.path + ".cova-restore-" + temporary_suffix());
    auto copied = fs::copy_file(directory / artifact.blob, temporary, true);
    if (!copied) {
      staged.push_back(Staged{directory / artifact.blob, destination, {temporary}});
      return fail_cleanup(copied.error());
    }
    staged.push_back(Staged{directory / artifact.blob, destination, {temporary}});
  }
  for (const auto& entry : staged) {
    auto renamed = fs::rename_path(entry.temporaries.front(), entry.destination);
    if (!renamed) {
      return fail_cleanup(renamed.error());
    }
  }
  return true;
}

}  // namespace cova::build
