// Cova — incremental build state and content-addressed artifact cache
// (PRD §9.3, §12.2, §12.3).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "build/action/action.hpp"
#include "core/common.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"

namespace cova::build {

namespace stdfs = std::filesystem;
using Path = stdfs::path;

// Versions of the persisted schemas. A mismatch is a miss, never a silent
// compatibility decision.
inline constexpr std::string_view kBuildStateSchemaVersion = "cova.build-state/4";
inline constexpr std::string_view kArtifactCacheSchemaVersion = "cova.artifact-cache/3";

// A content digest paired with the metadata sample it was computed from. The
// digest stays valid only while the sample is unchanged.
struct StampedDigest {
  std::string path;
  std::string digest;
  fs::FileStamp stamp;
};

// One persisted record of a successfully completed action.
struct ActionRecord {
  // Closure-free identity (argv, declared inputs, include listings, toolchain,
  // environment). Stable across a regenerated depfile, so the warm incremental
  // check does not depend on a content hash over the whole header closure.
  std::string plan_key;
  // Full identity, including every discovered input. Used for the shared cache
  // and for provenance; not needed to answer "is this up to date?".
  std::string cache_key;
  std::vector<std::string> output_paths;
  std::vector<std::string> input_paths;
  std::vector<std::pair<std::string, std::string>> input_hashes;
  std::vector<StampedDigest> input_digests;
  // Content hashes of the produced outputs, so a corrupt output that still
  // exists is not mistaken for a current one.
  std::vector<std::pair<std::string, std::string>> output_hashes;
  std::vector<StampedDigest> output_digests;
  std::string toolchain_fingerprint;
  std::string sysroot_fingerprint;
  std::uint64_t completed_at_ns = 0;
  std::uint32_t duration_ms = 0;
  bool succeeded = true;
};

// Per-build memo of content digests, keyed by path and validated by metadata
// sample. It is seeded from the incremental state and from every digest the
// build computes, so a warm build pays one stat per input instead of a read and
// a hash. A changed sample forces a real read, which is what keeps a touched
// file that still holds the same bytes from forcing a rebuild.
class FileIdentityCache {
 public:
  // Records a digest that is already known to be valid for `stamp`.
  void remember(const std::string& path, const fs::FileStamp& stamp, std::string digest);

  // Content digest as lowercase hex, or "missing" when the file cannot be read.
  // Re-reads only when the cached sample no longer matches the file. When
  // `stamp_out` is given it receives the sample the digest belongs to.
  const std::string& digest(const Path& path, fs::FileStamp* stamp_out = nullptr);

  // Directory listings are part of a compile action's identity. Every compile
  // in one build lists the same include directories, so the encoded listing is
  // computed once per directory per build.
  const std::vector<std::string>& listing(const Path& directory, bool& complete);

  std::size_t files_read() const { return files_read_; }
  std::size_t lookups() const { return lookups_; }
  // Number of recorded files whose metadata moved and whose digest therefore
  // had to be recomputed. Non-zero means the persisted state is out of date.
  std::size_t refreshed() const { return refreshed_; }

 private:
  struct Entry {
    fs::FileStamp stamp;
    std::string digest;
  };
  struct Listing {
    std::vector<std::string> entries;
    bool complete = true;
  };
  std::map<std::string, Entry> entries_;
  std::map<std::string, Listing> listings_;
  std::size_t files_read_ = 0;
  std::size_t lookups_ = 0;
  std::size_t refreshed_ = 0;
};

// The local incremental record beside target/.cova/state.toml. It is separate
// from the shared artifact cache on purpose: losing either is safe.
class BuildState {
 public:
  static Result<BuildState> load(const Path& path);
  Status save(const Path& path) const;

  // A record is current when its identity matches, every output is unchanged
  // and every recorded input still holds the recorded content. `cache` supplies
  // digests for files whose metadata sample has not moved; without it every
  // recorded file is hashed again.
  bool is_up_to_date(const Action& action, std::string& reason,
                     FileIdentityCache* cache = nullptr) const;
  // True when the recorded identity and every recorded input still match,
  // independent of whether the outputs exist. Used to avoid a dependency scan
  // when only an output needs rebuilding.
  bool inputs_match(const Action& action, FileIdentityCache* cache = nullptr) const;

  void record(ActionRecord record);
  std::size_t size() const { return records_.size(); }
  void clear() { records_.clear(); }

  const std::vector<ActionRecord>& records() const { return records_; }

  // The record that describes this action's build step, or nullptr.
  const ActionRecord* find(const Action& action) const;

 private:
  bool verify_digests(const std::vector<std::pair<std::string, std::string>>& hashes,
                      const std::vector<StampedDigest>& stamps, std::string_view role,
                      std::string& reason, FileIdentityCache* cache) const;

  std::vector<ActionRecord> records_;
};

// A depfile parsed from `-MMD -MF` output (PRD §9.3 header tracking).
struct Depfile {
  std::vector<Path> dependencies;
  static Result<Depfile> parse(std::string_view text, const Path& base);
  static Result<Depfile> load(const Path& path, const Path& base);
};

// Shared artifact cache. Entries are keyed by Action::cache_key and contain a
// versioned manifest plus the output blobs. Publication is atomic: the manifest
// is written last, so a partial entry is never observable. A second writer for
// the same key with different output content is a non-determinism incident and
// never overwrites the existing entry.
struct CacheSettings {
  Path root;
  bool enabled = true;
  bool read_only = false;
  std::uint64_t max_size_bytes = 8ull * 1024 * 1024 * 1024;
};

CacheSettings default_cache_settings();

// Restores the action's outputs from the cache. Returns false when there is no
// complete, schema-compatible, integrity-valid entry. Outputs are staged and
// renamed, so a failure cannot leave a half-restored artifact.
Result<bool> cache_restore(const CacheSettings& settings, const Action& action,
                           const std::string& key);
// Publishes a completed action. Returns a `cova::build::cache_nondeterminism`
// error when the same key already holds different output content.
Status cache_store(const CacheSettings& settings, const Action& action,
                   const std::string& key);

class FileHasher {
 public:
  Result<Digest> hash_file(const Path& path) const;
};

}  // namespace cova::build
