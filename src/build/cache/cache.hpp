// Cova — incremental build state and content-addressed artifact cache
// (PRD §9.3, §12.2, §12.3).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "build/action/action.hpp"
#include "core/common.hpp"
#include "core/hashing/hash.hpp"

namespace cova::build {

namespace stdfs = std::filesystem;
using Path = stdfs::path;

// Versions of the persisted schemas. A mismatch is a miss, never a silent
// compatibility decision.
inline constexpr std::string_view kBuildStateSchemaVersion = "cova.build-state/3";
inline constexpr std::string_view kArtifactCacheSchemaVersion = "cova.artifact-cache/2";

// One persisted record of a successfully completed action.
struct ActionRecord {
  std::string cache_key;
  std::vector<std::string> output_paths;
  std::vector<std::string> input_paths;
  std::vector<std::pair<std::string, std::string>> input_hashes;
  // Content hashes of the produced outputs, so a corrupt output that still
  // exists is not mistaken for a current one.
  std::vector<std::pair<std::string, std::string>> output_hashes;
  std::string toolchain_fingerprint;
  std::string sysroot_fingerprint;
  std::uint64_t completed_at_ns = 0;
  std::uint32_t duration_ms = 0;
  bool succeeded = true;
};

// The local incremental record beside target/.cova/state.toml. It is separate
// from the shared artifact cache on purpose: losing either is safe.
class BuildState {
 public:
  static Result<BuildState> load(const Path& path);
  Status save(const Path& path) const;

  // A record is current when its key matches, all outputs exist, and every
  // recorded input still has the recorded content hash.
  bool is_up_to_date(const Action& action, std::string& reason) const;
  // True when the recorded identity and every recorded input hash match the
  // action, independent of whether its outputs still exist. Used to avoid a
  // dependency scan when only an output needs rebuilding.
  bool inputs_match(const Action& action) const;

  void record(ActionRecord record);
  std::size_t size() const { return records_.size(); }
  void clear() { records_.clear(); }

  const std::vector<ActionRecord>& records() const { return records_; }

 private:
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
