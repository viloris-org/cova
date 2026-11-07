// Cova — filesystem utilities with safety guarantees (PRD §12.4, §18.1, §19.2).
//
// All destructive helpers validate their resolved absolute target first and
// refuse to leave the managed root. Symlinks are never followed when deleting.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova::fs {

namespace stdfs = std::filesystem;
using Path = stdfs::path;

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
bool exists(const Path& p);
bool is_directory(const Path& p);
bool is_regular_file(const Path& p);
bool is_symlink(const Path& p);
// Size in bytes; 0 when missing or not a regular file.
std::uint64_t file_size(const Path& p);
// Modification time as nanoseconds since the epoch; nullopt when unavailable.
std::optional<std::int64_t> mtime_ns(const Path& p);

// Reads a whole file as text.
Result<std::string> read_file(const Path& p);
// Reads a whole file as bytes.
Result<std::vector<std::byte>> read_bytes(const Path& p);
// Reads the first `limit` lines as text (used for tool version probing).
Result<std::string> read_file_head(const Path& p, std::size_t limit = 4096);

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------
// Writes via a sibling temporary file plus rename, so an interrupted write
// never leaves a half-written file at `p` (PRD §19.2).
Status write_file_atomic(const Path& p, std::string_view data);
Status write_file_atomic(const Path& p, std::span<const std::byte> data);
// Plain write; creates parent directories.
Status write_file(const Path& p, std::string_view data);

Status create_directories(const Path& p);
Status copy_file(const Path& from, const Path& to, bool overwrite = false);
Status copy_tree(const Path& from, const Path& to);
Status rename_path(const Path& from, const Path& to);
// Recursive delete inside an allowed root. Returns the number of entries removed.
Result<std::size_t> remove_tree(const Path& target, const Path& allowed_root);
Status remove_file(const Path& target);
// Deletes only when the path is a regular file inside `allowed_root`.
Status safe_remove(const Path& target, const Path& allowed_root);

// Creates a unique temporary directory under `parent`, prefixed by `prefix`.
Result<Path> make_temp_dir(const Path& parent, std::string_view prefix);

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------
struct GlobError {
  std::string pattern;
  std::string reason;
};

// Expands a Cova source glob such as "src/**/*.cpp" relative to `base`.
// Supports `*`, `**` and `?`; results are sorted and de-duplicated.
Result<std::vector<Path>> glob(const Path& base, std::string_view pattern);

// Recursively lists files under `root`, sorted, skipping hidden entries.
std::vector<Path> walk_files(const Path& root);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
// Lexically normalises a path without touching the filesystem.
Path normalize(const Path& p);
// Absolute path with lexically-normalised components (no symlink resolution).
Path absolute_normalized(const Path& p);
// Absolute, weakly-canonical path: resolves symlinks for the longest existing
// prefix and appends the remainder. Falls back to normalised absolute path.
Path canonical_for_compare(const Path& p);
// True when `child` is inside `root` after normalisation (no traversal).
bool is_within(const Path& child, const Path& root);
// Relative path with forward slashes, for stable output and lockfiles.
std::string generic_string(const Path& p);
// Joins with '/', skipping empty components.
std::string join_generic(std::string_view a, std::string_view b);

// Standard Cova directories for this host (PRD §8.3). Created lazily by
// `ensure_layout`.
struct CovaDirs {
  Path root;        // e.g. ~/.cova or $COVA_HOME
  Path toolchains;  // root/toolchains
  Path sysroots;    // root/sysroots
  Path store;       // root/store
  Path downloads;   // root/downloads
  Path registry;    // root/registry
  Path cache;       // root/cache
  Path config_file; // root/config.toml
};

// Honours $COVA_HOME first, then XDG_DATA_HOME/cova, then ~/.cova.
Result<CovaDirs> cova_dirs();
Status ensure_layout(const CovaDirs& dirs);

// Environment helpers.
std::optional<std::string> get_env(std::string_view name);
Status set_env(std::string_view name, std::string_view value);
// Splits $PATH into entries.
std::vector<Path> path_entries();
std::optional<Path> find_in_path(std::string_view executable);
// The current working directory.
Result<Path> current_dir();

}  // namespace cova::fs
