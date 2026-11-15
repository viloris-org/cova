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

// "<path>\t<mtime-ns>\t<size>\t<inode>\t<exists>\t<digest>". Parsed from the
// right so a path containing a tab survives; digests never contain one.
std::string encode_digest(const StampedDigest& digest) {
  return digest.path + "\t" + std::to_string(digest.stamp.mtime_ns) + "\t" +
         std::to_string(digest.stamp.size) + "\t" + std::to_string(digest.stamp.inode) +
         "\t" + (digest.stamp.exists ? "1" : "0") + "\t" + digest.digest;
}

std::vector<std::string> encode_digests(const std::vector<StampedDigest>& digests) {
  std::vector<std::string> encoded;
  encoded.reserve(digests.size());
  for (const auto& digest : digests) {
    encoded.push_back(encode_digest(digest));
  }
  return encoded;
}

std::vector<StampedDigest> decode_digests(const Table& table, std::string_view key) {
  std::vector<StampedDigest> digests;
  for (const auto& encoded : strings_or(table, key)) {
    // Split off the five fixed trailing fields, right to left.
    std::vector<std::string> fields;
    std::string rest = encoded;
    bool broken = false;
    for (int index = 0; index < 5; ++index) {
      const auto position = rest.rfind('\t');
      if (position == std::string::npos) {
        broken = true;
        break;
      }
      fields.push_back(rest.substr(position + 1));
      rest.resize(position);
    }
    if (broken || fields.size() != 5) {
      continue;
    }
    StampedDigest digest;
    digest.path = rest;
    digest.digest = fields[0];
    digest.stamp.exists = fields[1] == "1";
    try {
      digest.stamp.inode = std::stoull(fields[2]);
      digest.stamp.size = std::stoull(fields[3]);
      digest.stamp.mtime_ns = std::stoll(fields[4]);
    } catch (const std::exception&) {
      continue;
    }
    digests.push_back(std::move(digest));
  }
  return digests;
}

std::string directory_entry_kind(const std::filesystem::directory_entry& entry) {
  std::error_code ec;
  const auto status = entry.symlink_status(ec);
  if (ec) {
    return "?";
  }
  if (std::filesystem::is_symlink(status)) {
    return "l";
  }
  if (std::filesystem::is_directory(status)) {
    return "d";
  }
  if (std::filesystem::is_regular_file(status)) {
    return "f";
  }
  return "?";
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
// Content digests, memoised per build
// ---------------------------------------------------------------------------
void FileIdentityCache::remember(const std::string& path, const fs::FileStamp& stamp,
                                 std::string digest) {
  Entry& entry = entries_[path];
  entry.stamp = stamp;
  entry.digest = std::move(digest);
}

const std::string& FileIdentityCache::digest(const Path& path, fs::FileStamp* stamp_out) {
  const std::string key = fs::generic_string(path);
  ++lookups_;
  const fs::FileStamp current = fs::stamp(path);
  const auto recorded = entries_.find(key);
  if (recorded != entries_.end() && recorded->second.stamp == current) {
    if (stamp_out != nullptr) {
      *stamp_out = current;
    }
    return recorded->second.digest;
  }
  ++files_read_;
  if (recorded != entries_.end()) {
    // The file was recorded and its metadata moved, so the stored digest has to
    // be re-derived and the record refreshed.
    ++refreshed_;
  }
  std::string text = "missing";
  if (current.exists) {
    auto hashed = Sha256::hex_of_file(path);
    text = hashed ? *hashed : std::string("missing");
  }
  Entry& entry = entries_[key];
  entry.stamp = current;
  entry.digest = std::move(text);
  if (stamp_out != nullptr) {
    *stamp_out = current;
  }
  return entry.digest;
}

const std::vector<std::string>& FileIdentityCache::listing(const Path& directory, bool& complete) {
  const std::string key = fs::generic_string(directory);
  if (auto it = listings_.find(key); it != listings_.end()) {
    complete = it->second.complete;
    return it->second.entries;
  }
  Listing listing;
  std::error_code ec;
  const bool is_directory = stdfs::is_directory(directory, ec);
  if (ec) {
    // The directory status could not be established (for example a permission
    // error on a parent). This is not equivalent to a missing directory.
    listing.entries.push_back("directory-check-incomplete");
    listing.complete = false;
  } else if (!is_directory) {
    // A missing include directory is complete evidence: its listing is empty.
    // If the directory appears later, the key changes.
    listing.entries.push_back("directory-missing");
  } else {
    stdfs::directory_iterator it(directory, ec);
    if (ec) {
      listing.complete = false;
    }
    const stdfs::directory_iterator end;
    while (!ec && it != end) {
      const std::string name = it->path().filename().string();
      const std::string kind = directory_entry_kind(*it);
      if (kind == "?") {
        // An entry whose type cannot be established makes the listing partial.
        listing.complete = false;
      }
      std::string line = name;
      line.push_back('\t');
      line += kind;
      std::error_code link_error;
      if (stdfs::is_symlink(it->path(), link_error)) {
        if (link_error) {
          listing.complete = false;
        }
        std::error_code target_error;
        const Path target = stdfs::read_symlink(it->path(), target_error);
        line += "\t->";
        line += target_error ? std::string("<unreadable>") : fs::generic_string(target);
        if (target_error) {
          listing.complete = false;
        }
      }
      listing.entries.push_back(std::move(line));
      it.increment(ec);
    }
    if (ec) {
      // A partial listing is not a proof of the input closure. Keep the entries
      // for diagnostics, but mark the identity incomplete so nothing is reused.
      listing.complete = false;
      listing.entries.push_back("directory-enumeration-incomplete");
    }
  }
  std::sort(listing.entries.begin(), listing.entries.end());
  complete = listing.complete;
  Listing& stored = listings_[key];
  stored = std::move(listing);
  return stored.entries;
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
      record.plan_key = string_or(table, "plan-key");
      record.cache_key = string_or(table, "cache-key");
      record.output_paths = strings_or(table, "output-paths");
      record.input_paths = strings_or(table, "input-paths");
      for (const auto& encoded : strings_or(table, "output-hashes")) {
        const auto position = encoded.rfind('\t');
        if (position == std::string::npos) {
          continue;
        }
        record.output_hashes.emplace_back(encoded.substr(0, position),
                                          encoded.substr(position + 1));
      }
      record.output_digests = decode_digests(table, "output-digests");
      record.input_digests = decode_digests(table, "input-digests");
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
    if (!record.plan_key.empty()) {
      table.emplace("plan-key", Value(record.plan_key));
    }
    table.emplace("cache-key", Value(record.cache_key));
    put_strings(table, "output-paths", record.output_paths);
    put_strings(table, "input-paths", record.input_paths);
    std::vector<std::string> hashes;
    hashes.reserve(record.input_hashes.size());
    for (const auto& [input, digest] : record.input_hashes) {
      hashes.push_back(input + "\t" + digest);
    }
    put_strings(table, "input-hashes", hashes);
    put_strings(table, "input-digests", encode_digests(record.input_digests));
    put_strings(table, "output-digests", encode_digests(record.output_digests));
    std::vector<std::string> output_hashes;
    output_hashes.reserve(record.output_hashes.size());
    for (const auto& [output, digest] : record.output_hashes) {
      output_hashes.push_back(output + "\t" + digest);
    }
    put_strings(table, "output-hashes", output_hashes);
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

const ActionRecord* BuildState::find(const Action& action) const {
  // The plan key answers "is this the same build step?" without hashing the
  // header closure, so it is the primary identity. A record from an older
  // schema (or a hand-built one) may only carry the full key.
  if (!action.computed_plan_key.empty()) {
    for (const auto& record : records_) {
      if (record.plan_key == action.computed_plan_key) {
        return &record;
      }
    }
  }
  if (!action.computed_key.empty()) {
    for (const auto& record : records_) {
      if (record.cache_key == action.computed_key) {
        return &record;
      }
    }
  }
  return nullptr;
}

bool BuildState::verify_digests(
    const std::vector<std::pair<std::string, std::string>>& hashes,
    const std::vector<StampedDigest>& stamps, std::string_view role,
    std::string& reason, FileIdentityCache* cache) const {
  FileHasher hasher;
  for (std::size_t index = 0; index < hashes.size(); ++index) {
    const std::string& path = hashes[index].first;
    const std::string& expected = hashes[index].second;
    std::string actual;
    if (cache != nullptr) {
      actual = cache->digest(Path(path));
    } else {
      auto digest = hasher.hash_file(path);
      actual = digest ? cova::to_hex(*digest) : std::string("missing");
    }
    if (actual == "missing") {
      reason = std::string(role) + " is missing: " + path;
      return false;
    }
    if (actual != expected) {
      reason = std::string(role) + " changed: " + path;
      return false;
    }
  }
  return true;
}

bool BuildState::is_up_to_date(const Action& action, std::string& reason,
                               FileIdentityCache* cache) const {
  if (action.computed_plan_key.empty() && action.computed_key.empty()) {
    // Output-path matching without an action identity is not a correctness
    // proof. Callers must compute the identity first.
    reason = "no action identity";
    return false;
  }
  const ActionRecord* match = find(action);
  if (match == nullptr) {
    reason = "no previous build record";
    return false;
  }
  if (!match->succeeded) {
    reason = "the previous build failed";
    return false;
  }
  for (const auto& output : match->output_paths) {
    if (!fs::is_regular_file(output)) {
      reason = "output is missing: " + output;
      return false;
    }
  }
  if (match->output_hashes.size() != match->output_paths.size()) {
    reason = "the previous record has no complete output integrity data";
    return false;
  }
  if (!verify_digests(match->output_hashes, match->output_digests, "output", reason, cache)) {
    return false;
  }
  if (!verify_digests(match->input_hashes, match->input_digests, "input", reason, cache)) {
    return false;
  }
  reason = "up to date";
  return true;
}

bool BuildState::inputs_match(const Action& action, FileIdentityCache* cache) const {
  const ActionRecord* match = find(action);
  if (match == nullptr || !match->succeeded) {
    return false;
  }
  std::string reason;
  return verify_digests(match->input_hashes, match->input_digests, "input", reason, cache);
}

void BuildState::record(ActionRecord record) {
  if (!record.plan_key.empty()) {
    for (auto& existing : records_) {
      if (existing.plan_key == record.plan_key) {
        existing = std::move(record);
        return;
      }
    }
  }
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
  // GNU make / GCC / Clang dependency-file syntax:
  //
  //   target: dep dep   //     dep
  //
  // with `\ ` and `\#` escaping whitespace/special characters inside a path,
  // `\:` escaping a colon, and a `C:\dir` drive prefix that is *not* the
  // target separator. Windows compilers emit raw backslashes, so a parser that
  // treats `\` as "always escape the next byte" mangles every dependency
  // (`C:\src\a.cpp` became `C:src a.cpp`).
  Depfile depfile;

  // Join continuation lines. A trailing backslash continues the logical line;
  // CRLF endings are stripped so `\r` never becomes part of a path.
  std::string joined;
  for (const auto& raw_line : str::split(text, '\n')) {
    std::string line = raw_line;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    // A continuation is a *single* backslash at end of line, not an escaped one.
    std::size_t trailing = 0;
    while (trailing < line.size() && line[line.size() - 1 - trailing] == '\\') {
      ++trailing;
    }
    if (trailing % 2 == 1) {
      line.pop_back();
      joined += line;
      joined.push_back(' ');
    } else {
      joined += line;
      joined.push_back('\n');
    }
  }

  // Find the target/dependency separator, skipping Windows drive prefixes.
  // A drive prefix is a single letter followed by ':' and then a separator, and
  // it can only occur at the very start of a path token (beginning of the file
  // or right after whitespace).
  std::size_t colon = std::string::npos;
  for (std::size_t index = 0; index < joined.size(); ++index) {
    if (joined[index] != ':') {
      continue;
    }
    // The drive letter sits at `index - 1`, so the token starts one byte before
    // that: either `index == 1`, or `index - 2` is whitespace.
    const bool at_token_start =
        index >= 1 && (index == 1 ||
                       std::isspace(static_cast<unsigned char>(joined[index - 2])) != 0);
    const bool looks_like_drive =
        at_token_start && std::isalpha(static_cast<unsigned char>(joined[index - 1])) &&
        index + 1 < joined.size() &&
        (joined[index + 1] == '/' || joined[index + 1] == '\\');
    if (looks_like_drive) {
      continue;
    }
    colon = index;
    break;
  }
  if (colon == std::string::npos) {
    return fail<Depfile>(Error::make("cova::build::depfile", "dependency file has no target line"));
  }

  const std::string dependencies = joined.substr(colon + 1);
  std::string current;
  bool have_token = false;
  auto flush = [&]() {
    if (have_token && !current.empty()) {
      depfile.dependencies.push_back(base / current);
    }
    current.clear();
    have_token = false;
  };

  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    const char character = dependencies[index];
    if (character == '\\' && index + 1 < dependencies.size()) {
      const char next = dependencies[index + 1];
      // `\ ` and `\#` and `\\` are escapes; a backslash before anything else
      // (notably before a drive-letter path) is literal, because Windows paths
      // contain far more backslashes than escapes.
      if (next == ' ' || next == '\t' || next == '#' || next == '\\' || next == ':') {
        current.push_back(next);
        have_token = true;
        ++index;
        continue;
      }
      current.push_back(character);
      have_token = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      flush();
      continue;
    }
    current.push_back(character);
    have_token = true;
  }
  flush();
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

bool is_plain_component(std::string_view text) {
  if (text.empty() || text == "." || text == "..") {
    return false;
  }
  return text.find('/') == std::string::npos && text.find('\\') == std::string::npos;
}

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
    if (!is_plain_component(artifact.blob)) {
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

Table artifact_manifest(const std::string& key, const std::vector<Artifact>& artifacts,
                         const std::string& depfile_path, const std::string& depfile_text) {
  Table top;
  top.emplace("schema-version", Value(std::string(kArtifactCacheSchemaVersion)));
  top.emplace("key", Value(key));
  if (!depfile_text.empty()) {
    Table depfile;
    depfile.emplace("path", Value(depfile_path));
    depfile.emplace("text", Value(depfile_text));
    top.emplace("depfile", Value(std::move(depfile)));
  }
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
  if (!settings.enabled || settings.read_only || action.outputs.empty()) {
    return ok();
  }
  if (!is_plain_component(key)) {
    return fail(Error::make("cova::build::cache_key", "invalid artifact cache key")
                    .with("key: " + key));
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

  // A restored tree must also get the dependency record the compiler wrote:
  // without it the next build cannot prove the input closure, would rescan, and
  // would not notice a header change until that scan. Persisting it beside the
  // outputs makes a restored tree indistinguishable from a compiled one.
  std::string depfile_path;
  std::string depfile_text;
  if (action.kind == ActionKind::Compile && !action.outputs.empty()) {
    Path depfile = action.outputs.front().path;
    depfile += ".d";
    if (auto text = fs::read_file(depfile)) {
      depfile_path = fs::generic_string(depfile);
      depfile_text = std::move(*text);
    }
  }
  auto manifest = fs::write_file_atomic(
      directory / "manifest.toml",
      toml::write(artifact_manifest(key, *artifacts, depfile_path, depfile_text)));
  if (!manifest) {
    cleanup();
    return manifest;
  }
  completed = true;
  return ok();
}

Result<bool> cache_restore(const CacheSettings& settings, const Action& action,
                           const std::string& key) {
  if (!settings.enabled || action.outputs.empty() || !is_plain_component(key)) {
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
    if (artifact.path.empty() || artifact.blob.empty() || artifact.sha256.empty() ||
        !is_plain_component(artifact.blob)) {
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
  if (const Value* depfile = get(top, "depfile"); depfile != nullptr && depfile->is_table()) {
    const Table& table = *depfile->as_table();
    const std::string path = string_or(table, "path");
    const std::string text = string_or(table, "text");
    if (!path.empty() && !text.empty()) {
      // Best effort: the outputs are already in place, and a tree without a
      // depfile is still correct - it only costs the next build a rescan.
      const Path destination(path);
      if (destination.has_parent_path()) {
        (void)fs::create_directories(destination.parent_path());
      }
      (void)fs::write_file_atomic(destination, text);
    }
  }
  return true;
}

}  // namespace cova::build
