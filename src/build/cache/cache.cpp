// Cova — incremental build state and artifact cache.
#include "build/cache/cache.hpp"

#include <algorithm>
#include <cctype>
#include <string>
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

std::string cache_entry_dir(const CacheSettings& settings, const std::string& key) {
  const std::string prefix = key.size() >= 2 ? key.substr(0, 2) : "00";
  return fs::generic_string(settings.root / "actions" / prefix / key);
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
  top.emplace("record", Value(std::move(records)));
  return fs::write_file_atomic(path, toml::write(top));
}

bool BuildState::is_up_to_date(const Action& action, std::string& reason) const {
  const ActionRecord* match = nullptr;
  if (!action.computed_key.empty()) {
    for (const auto& record : records_) {
      if (record.cache_key == action.computed_key) {
        match = &record;
        break;
      }
    }
  } else {
    // Without a key we can still match the output set. This is only used by
    // callers that deliberately skip key computation.
    std::vector<std::string> outputs;
    for (const auto& output : action.outputs) {
      outputs.push_back(fs::generic_string(output.path));
    }
    for (const auto& record : records_) {
      if (record.output_paths == outputs) {
        match = &record;
        break;
      }
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

Status cache_store(const CacheSettings& settings, const Action& action,
                   const std::string& key) {
  if (!settings.enabled || settings.read_only || action.outputs.empty()) {
    return ok();
  }
  const auto directory = Path(cache_entry_dir(settings, key));
  auto created = fs::create_directories(directory);
  if (!created) {
    return created;
  }
  Table top;
  top.emplace("key", Value(key));
  toml::Array outputs;
  std::size_t index = 0;
  for (const auto& output : action.outputs) {
    if (!fs::is_regular_file(output.path)) {
      return fail(Error::make("cova::build::cache_store",
                              "cannot cache a missing output: " +
                                  fs::generic_string(output.path)));
    }
    const std::string blob_name = "blob-" + std::to_string(index);
    auto copied = fs::copy_file(output.path, directory / blob_name, true);
    if (!copied) {
      return copied;
    }
    Table entry;
    entry.emplace("path", Value(fs::generic_string(output.path)));
    entry.emplace("blob", Value(blob_name));
    auto digest = Sha256::hex_of_file(output.path);
    if (digest) {
      entry.emplace("sha256", Value(*digest));
    }
    outputs.emplace_back(Value(std::move(entry)));
    ++index;
  }
  top.emplace("output", Value(std::move(outputs)));
  return fs::write_file_atomic(directory / "manifest.toml", toml::write(top));
}

Result<bool> cache_restore(const CacheSettings& settings, const Action& action,
                           const std::string& key) {
  if (!settings.enabled || action.outputs.empty()) {
    return false;
  }
  const auto directory = Path(cache_entry_dir(settings, key));
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
  const Value* outputs = get(top, "output");
  if (outputs == nullptr || !outputs->is_array()) {
    return false;
  }
  // Validate every blob before touching any destination, so a partial cache
  // entry can never leave a half-restored build.
  struct Restore {
    Path from;
    Path to;
  };
  std::vector<Restore> restores;
  for (const auto& entry : *outputs->as_array()) {
    if (!entry.is_table()) {
      return false;
    }
    const Table& table = *entry.as_table();
    const std::string path = string_or(table, "path");
    const std::string blob = string_or(table, "blob");
    if (path.empty() || blob.empty()) {
      return false;
    }
    const auto from = directory / blob;
    if (!fs::is_regular_file(from)) {
      return false;
    }
    const std::string expected = string_or(table, "sha256");
    if (!expected.empty()) {
      auto digest = Sha256::hex_of_file(from);
      if (!digest || *digest != expected) {
        return false;
      }
    }
    restores.push_back(Restore{from, Path(path)});
  }
  for (const auto& restore : restores) {
    if (restore.to.has_parent_path()) {
      auto created = fs::create_directories(restore.to.parent_path());
      if (!created) {
        return fail<bool>(created.error());
      }
    }
    auto copied = fs::copy_file(restore.from, restore.to, true);
    if (!copied) {
      return fail<bool>(copied.error());
    }
  }
  return true;
}

}  // namespace cova::build
