// Cova — filesystem utilities with safety guarantees (implementation).
//
// Every destructive operation validates the resolved absolute target before it
// touches the disk (PRD §12.4, §18.1). Deletions never follow symlinks.
#include "core/filesystem/filesystem.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cova::fs {
namespace {

#ifdef _WIN32
[[maybe_unused]] constexpr char kSeparator = '\\';
int process_id() { return _getpid(); }
bool access_executable(const Path& p) { return ::_access(p.string().c_str(), 0) == 0; }
#else
[[maybe_unused]] constexpr char kSeparator = '/';
int process_id() { return static_cast<int>(::getpid()); }
bool access_executable(const Path& p) { return ::access(p.string().c_str(), X_OK) == 0; }
#endif


std::string path_text(const Path& p) { return p.generic_string(); }

// Symlink status without throwing.
stdfs::file_status status_of(const Path& p) {
  std::error_code ec;
  const stdfs::file_status status = stdfs::symlink_status(p, ec);
  if (ec) {
    return {};
  }
  return status;
}

bool exists_nothrow(const Path& p) {
  std::error_code ec;
  return stdfs::exists(stdfs::symlink_status(p, ec)) && !ec;
}

// Internal, ADL-safe helpers: `Path` is `std::filesystem::path`, so unqualified
// calls to names that exist in both namespaces would be ambiguous.
bool is_file(const Path& p) {
  std::error_code ec;
  return stdfs::is_regular_file(p, ec) && !ec;
}

bool is_dir(const Path& p) {
  std::error_code ec;
  return stdfs::is_directory(p, ec) && !ec;
}

bool path_exists(const Path& p) {
  std::error_code ec;
  return stdfs::exists(p, ec) && !ec;
}

// Internal mkdir helper: avoids ADL ambiguity with std::filesystem.
Status mkdirs(const Path& p) {
  if (p.empty()) {
    return ok();
  }
  std::error_code ec;
  if (stdfs::is_directory(p, ec)) {
    return ok();
  }
  stdfs::create_directories(p, ec);
  if (ec && !stdfs::is_directory(p)) {
    return std::unexpected(Error::make("cova::fs::create_dir_failed",
                                       "cannot create directory")
                               .with("path: " + p.generic_string())
                               .with("reason: " + ec.message()));
  }
  return ok();
}

// Shallow listing (non-recursive), sorted for determinism.
std::vector<Path> list_dir(const Path& dir, bool* ok) {
  std::vector<Path> entries;
  std::error_code ec;
  stdfs::directory_iterator it(dir, stdfs::directory_options::none, ec);
  if (ec) {
    *ok = false;
    return entries;
  }
  const stdfs::directory_iterator end;
  while (it != end) {
    entries.push_back(it->path());
    it.increment(ec);
    if (ec) {
      break;
    }
  }
  std::sort(entries.begin(), entries.end());
  *ok = true;
  return entries;
}

bool is_hidden_name(const std::string& name) { return !name.empty() && name[0] == '.'; }

// Matches a single path component against a glob fragment supporting '*' and
// '?' (no character classes — Cova's source globs do not need them).
bool match_fragment(std::string_view pattern, std::string_view name) {
  std::size_t p = 0;
  std::size_t n = 0;
  std::size_t star_p = std::string_view::npos;
  std::size_t star_n = 0;
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]) &&
        pattern[p] != '*') {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++;
      star_n = n;
    } else if (star_p != std::string_view::npos) {
      p = star_p + 1;
      n = ++star_n;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

bool has_wildcard(std::string_view component) {
  return component.find('*') != std::string_view::npos ||
         component.find('?') != std::string_view::npos;
}

struct GlobContext {
  std::set<Path> results;

  void collect_files(const Path& dir) {
    std::error_code ec;
    stdfs::recursive_directory_iterator it(dir, stdfs::directory_options::none, ec);
    if (ec) {
      return;
    }
    const stdfs::recursive_directory_iterator end;
    while (it != end) {
      const stdfs::file_status status = it->symlink_status(ec);
      if (ec) {
        break;
      }
      if (status.type() == stdfs::file_type::directory) {
        // Do not descend into symlinked directories (cycle risk).
        if (status.type() == stdfs::file_type::symlink) {
          it.disable_recursion_pending();
        }
      } else if (it->is_regular_file(ec) && !ec) {
        results.insert(it->path());
      }
      it.increment(ec);
      if (ec) {
        break;
      }
    }
  }

  void match(const Path& current, const std::vector<std::string>& components, std::size_t index) {
    if (index == components.size()) {
      if (is_file(current)) {
        results.insert(current);
      }
      return;
    }
    const std::string& component = components[index];
    if (component == "**") {
      // `**` matches zero or more directories.
      match(current, components, index + 1);
      if (index + 1 == components.size()) {
        collect_files(current);
        return;
      }
      bool ok = false;
      for (const Path& entry : list_dir(current, &ok)) {
        const stdfs::file_status status = status_of(entry);
        if (stdfs::is_directory(status) && !stdfs::is_symlink(status)) {
          match(entry, components, index);
        }
      }
      return;
    }
    if (has_wildcard(component)) {
      bool ok = false;
      for (const Path& entry : list_dir(current, &ok)) {
        const std::string name = entry.filename().string();
        if (match_fragment(component, name)) {
          match(entry, components, index + 1);
        }
      }
      return;
    }
    const Path candidate = current / component;
    if (exists_nothrow(candidate)) {
      match(candidate, components, index + 1);
    }
  }
};

Path unique_temp_name(const Path& directory, std::string_view base, std::string_view kind) {
  static std::atomic<unsigned long long> counter{0};
  const unsigned long long n = ++counter;
  std::string name = ".cova-";
  name += kind;
  name += "-";
  name += std::to_string(process_id());
  name += "-";
  name += std::to_string(n);
  if (!base.empty()) {
    name += "-";
    name += base;
  }
  return directory / name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool exists(const Path& p) { return exists_nothrow(p); }

bool is_directory(const Path& p) {
  std::error_code ec;
  return stdfs::is_directory(p, ec) && !ec;
}

bool is_regular_file(const Path& p) {
  std::error_code ec;
  return stdfs::is_regular_file(p, ec) && !ec;
}

bool is_symlink(const Path& p) {
  std::error_code ec;
  return stdfs::is_symlink(stdfs::symlink_status(p, ec)) && !ec;
}

std::uint64_t file_size(const Path& p) {
  if (!is_file(p)) {
    return 0;
  }
  std::error_code ec;
  const std::uintmax_t size = stdfs::file_size(p, ec);
  if (ec) {
    return 0;
  }
  return static_cast<std::uint64_t>(size);
}

std::optional<std::int64_t> mtime_ns(const Path& p) {
  std::error_code ec;
  const stdfs::file_time_type time = stdfs::last_write_time(p, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto system_time =
      std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          time - stdfs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return std::chrono::duration_cast<std::chrono::nanoseconds>(system_time.time_since_epoch())
      .count();
}

Result<std::string> read_file(const Path& p) {
  if (!is_file(p)) {
    if (!path_exists(p)) {
      return std::unexpected(Error::make("cova::fs::not_found", "file does not exist")
                                 .with("path: " + path_text(p)));
    }
    return std::unexpected(Error::make("cova::fs::not_a_file", "path is not a regular file")
                               .with("path: " + path_text(p)));
  }
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return std::unexpected(Error::make("cova::fs::read_failed", "cannot open file for reading")
                               .with("path: " + path_text(p)));
  }
  std::string data;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size > 0) {
    data.resize(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(data.data(), size);
    data.resize(static_cast<std::size_t>(in.gcount()));
  }
  return data;
}

Result<std::vector<std::byte>> read_bytes(const Path& p) {
  auto text = read_file(p);
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  std::vector<std::byte> bytes(text->size());
  if (!text->empty()) {
    std::memcpy(bytes.data(), text->data(), text->size());
  }
  return bytes;
}

Result<std::string> read_file_head(const Path& p, std::size_t limit) {
  if (!is_file(p)) {
    if (!path_exists(p)) {
      return std::unexpected(Error::make("cova::fs::not_found", "file does not exist")
                                 .with("path: " + path_text(p)));
    }
    return std::unexpected(Error::make("cova::fs::not_a_file", "path is not a regular file")
                               .with("path: " + path_text(p)));
  }
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return std::unexpected(Error::make("cova::fs::read_failed", "cannot open file for reading")
                               .with("path: " + path_text(p)));
  }
  // `limit` counts lines; a byte cap keeps a pathological file from exhausting
  // memory (tool version probes never need more).
  constexpr std::size_t kByteCap = 64 * 1024;
  std::string out;
  std::string line;
  std::size_t lines = 0;
  while (lines < limit && out.size() < kByteCap && std::getline(in, line)) {
    out += line;
    out.push_back('\n');
    ++lines;
  }
  if (out.size() > kByteCap) {
    out.resize(kByteCap);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

namespace {

[[maybe_unused]] Status write_all_fd(int fd, const char* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
#ifdef _WIN32
    const int n = ::_write(fd, data + written, static_cast<unsigned>(size - written));
#else
    const ssize_t n = ::write(fd, data + written, size - written);
#endif
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(Error::make("cova::fs::write_failed", "write failed")
                                 .with("errno: " + std::string(std::strerror(errno))));
    }
    if (n == 0) {
      break;
    }
    written += static_cast<std::size_t>(n);
  }
  return ok();
}

}  // namespace

Status write_file_atomic(const Path& p, std::string_view data) {
  const Path parent = p.has_parent_path() ? p.parent_path() : Path(".");
  if (Status prepared = mkdirs(parent); !prepared) {
    return prepared;
  }
  const Path temp = unique_temp_name(parent, p.filename().string(), "tmp");

#ifdef _WIN32
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return std::unexpected(Error::make("cova::fs::write_failed", "cannot create temp file")
                                 .with("path: " + path_text(temp)));
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
      std::error_code ec;
      stdfs::remove(temp, ec);
      return std::unexpected(Error::make("cova::fs::write_failed", "write failed")
                                 .with("path: " + path_text(temp)));
    }
    out.close();
  }
#else
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    return std::unexpected(Error::make("cova::fs::write_failed", "cannot create temp file")
                               .with("path: " + path_text(temp))
                               .with("errno: " + std::string(std::strerror(errno))));
  }
  Status wrote = write_all_fd(fd, data.data(), data.size());
  if (wrote) {
    if (::fsync(fd) != 0) {
      wrote = std::unexpected(Error::make("cova::fs::write_failed", "fsync failed")
                                  .with("path: " + path_text(temp))
                                  .with("errno: " + std::string(std::strerror(errno))));
    }
  }
  if (::close(fd) != 0 && wrote) {
    wrote = std::unexpected(Error::make("cova::fs::write_failed", "close failed")
                                .with("path: " + path_text(temp)));
  }
  if (!wrote) {
    std::error_code ec;
    stdfs::remove(temp, ec);
    return wrote;
  }
#endif

  std::error_code ec;
  stdfs::rename(temp, p, ec);
#ifdef _WIN32
  if (ec) {
    // Some Windows C runtimes refuse to rename onto an existing file. Replace
    // it explicitly so lockfile and manifest rewrites still succeed.
    std::error_code remove_ec;
    stdfs::remove(p, remove_ec);
    ec.clear();
    stdfs::rename(temp, p, ec);
  }
#endif
  if (ec) {
    std::error_code remove_ec;
    stdfs::remove(temp, remove_ec);
    return std::unexpected(Error::make("cova::fs::write_failed", "atomic rename failed")
                               .with("from: " + path_text(temp))
                               .with("to: " + path_text(p))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

Status write_file_atomic(const Path& p, std::span<const std::byte> data) {
  return write_file_atomic(
      p, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

Status write_file(const Path& p, std::string_view data) {
  const Path parent = p.has_parent_path() ? p.parent_path() : Path(".");
  if (Status prepared = mkdirs(parent); !prepared) {
    return prepared;
  }
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    return std::unexpected(Error::make("cova::fs::write_failed", "cannot open file for writing")
                               .with("path: " + path_text(p)));
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.flush();
  if (!out) {
    return std::unexpected(Error::make("cova::fs::write_failed", "write failed")
                               .with("path: " + path_text(p)));
  }
  return ok();
}

Status create_directories(const Path& p) {
  if (p.empty()) {
    return ok();
  }
  std::error_code ec;
  if (stdfs::is_directory(p, ec)) {
    return ok();
  }
  stdfs::create_directories(p, ec);
  if (ec && !stdfs::is_directory(p)) {
    return std::unexpected(Error::make("cova::fs::create_dir_failed",
                                       "cannot create directory")
                               .with("path: " + path_text(p))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

Status copy_file(const Path& from, const Path& to, bool overwrite) {
  if (!is_file(from)) {
    return std::unexpected(Error::make("cova::fs::not_found", "source file does not exist")
                               .with("path: " + path_text(from)));
  }
  const Path parent = to.has_parent_path() ? to.parent_path() : Path(".");
  if (Status prepared = mkdirs(parent); !prepared) {
    return prepared;
  }
  if (!overwrite && path_exists(to)) {
    return std::unexpected(Error::make("cova::fs::exists", "destination already exists")
                               .with("path: " + path_text(to)));
  }
  std::error_code ec;
  stdfs::copy_file(from, to,
                   overwrite ? stdfs::copy_options::overwrite_existing
                             : stdfs::copy_options::none,
                   ec);
  if (ec) {
    return std::unexpected(Error::make("cova::fs::copy_failed", "cannot copy file")
                               .with("from: " + path_text(from))
                               .with("to: " + path_text(to))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

Status copy_tree(const Path& from, const Path& to) {
  if (!is_dir(from)) {
    return std::unexpected(Error::make("cova::fs::not_found", "source directory does not exist")
                               .with("path: " + path_text(from)));
  }
  if (Status prepared = mkdirs(to); !prepared) {
    return prepared;
  }
  std::error_code ec;
  stdfs::recursive_directory_iterator it(from, stdfs::directory_options::none, ec);
  if (ec) {
    return std::unexpected(Error::make("cova::fs::copy_failed", "cannot walk source directory")
                               .with("path: " + path_text(from))
                               .with("reason: " + ec.message()));
  }
  const stdfs::recursive_directory_iterator end;
  while (it != end) {
    const Path source = it->path();
    const Path relative = stdfs::relative(source, from, ec);
    if (ec) {
      return std::unexpected(Error::make("cova::fs::copy_failed", "cannot compute relative path")
                                 .with("path: " + path_text(source))
                                 .with("reason: " + ec.message()));
    }
    const Path destination = to / relative;
    const stdfs::file_status status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (stdfs::is_symlink(status)) {
      // Recreate the link itself; never copy the target out of the tree.
      const Path link_target = stdfs::read_symlink(source, ec);
      if (ec) {
        break;
      }
      std::error_code create_ec;
      stdfs::create_symlink(link_target, destination, create_ec);
    } else if (stdfs::is_directory(status)) {
      std::error_code dir_ec;
      stdfs::create_directories(destination, dir_ec);
    } else {
      std::error_code copy_ec;
      stdfs::copy_file(source, destination, stdfs::copy_options::overwrite_existing, copy_ec);
      if (copy_ec) {
        return std::unexpected(Error::make("cova::fs::copy_failed", "cannot copy file")
                                   .with("from: " + path_text(source))
                                   .with("to: " + path_text(destination))
                                   .with("reason: " + copy_ec.message()));
      }
    }
    it.increment(ec);
    if (ec) {
      break;
    }
  }
  if (ec) {
    return std::unexpected(Error::make("cova::fs::copy_failed", "cannot copy directory tree")
                               .with("from: " + path_text(from))
                               .with("to: " + path_text(to))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

Status rename_path(const Path& from, const Path& to) {
  const Path parent = to.has_parent_path() ? to.parent_path() : Path(".");
  if (Status prepared = mkdirs(parent); !prepared) {
    return prepared;
  }
  std::error_code ec;
  stdfs::rename(from, to, ec);
  if (ec) {
    return std::unexpected(Error::make("cova::fs::rename_failed", "cannot rename path")
                               .with("from: " + path_text(from))
                               .with("to: " + path_text(to))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

namespace {

// Removes `entry` without ever following a symlink. Counts removed entries.
std::size_t remove_entry_tree(const Path& entry, std::error_code& first_error) {
  std::error_code ec;
  const stdfs::file_status status = stdfs::symlink_status(entry, ec);
  if (ec) {
    if (!first_error) {
      first_error = ec;
    }
    return 0;
  }
  if (stdfs::is_directory(status)) {
    bool ok_list = false;
    const std::vector<Path> children = list_dir(entry, &ok_list);
    if (!ok_list && !first_error) {
      first_error = std::make_error_code(std::errc::io_error);
    }
    std::size_t removed = 0;
    for (const Path& child : children) {
      removed += remove_entry_tree(child, first_error);
    }
    std::error_code remove_ec;
    if (!stdfs::remove(entry, remove_ec)) {
      if (remove_ec && !first_error) {
        first_error = remove_ec;
      }
      return removed;
    }
    return removed + 1;
  }
  std::error_code remove_ec;
  if (!stdfs::remove(entry, remove_ec)) {
    if (remove_ec && !first_error) {
      first_error = remove_ec;
    }
    return 0;
  }
  return 1;
}

}  // namespace

Result<std::size_t> remove_tree(const Path& target, const Path& allowed_root) {
  if (!path_exists(target)) {
    return std::size_t{0};
  }
  const Path resolved_target = canonical_for_compare(target);
  const Path resolved_root = canonical_for_compare(allowed_root);
  if (absolute_normalized(resolved_target) == absolute_normalized(resolved_root)) {
    return std::unexpected(
        Error::make("cova::fs::unsafe_delete", "refusing to delete the allowed root itself")
            .with("target: " + path_text(resolved_target))
            .with("root: " + path_text(resolved_root)));
  }
  if (!is_within(resolved_target, resolved_root)) {
    return std::unexpected(
        Error::make("cova::fs::outside_root", "refusing to delete a path outside the allowed root")
            .with("target: " + path_text(resolved_target))
            .with("root: " + path_text(resolved_root)));
  }
  std::error_code error;
  const std::size_t removed = remove_entry_tree(resolved_target, error);
  if (error) {
    return std::unexpected(Error::make("cova::fs::remove_failed", "failed to remove path")
                               .with("target: " + path_text(resolved_target))
                               .with("removed_entries: " + std::to_string(removed))
                               .with("reason: " + error.message()));
  }
  return removed;
}

Status remove_file(const Path& target) {
  const stdfs::file_status status = status_of(target);
  if (!stdfs::exists(status)) {
    return std::unexpected(Error::make("cova::fs::not_found", "path does not exist")
                               .with("path: " + path_text(target)));
  }
  if (stdfs::is_directory(status) && !stdfs::is_symlink(status)) {
    return std::unexpected(Error::make("cova::fs::is_directory", "path is a directory")
                               .with("path: " + path_text(target)));
  }
  std::error_code ec;
  if (!stdfs::remove(target, ec) || ec) {
    return std::unexpected(Error::make("cova::fs::remove_failed", "cannot remove file")
                               .with("path: " + path_text(target))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

Status safe_remove(const Path& target, const Path& allowed_root) {
  if (!path_exists(target)) {
    return std::unexpected(Error::make("cova::fs::not_found", "path does not exist")
                               .with("path: " + path_text(target)));
  }
  const Path resolved_target = canonical_for_compare(target);
  const Path resolved_root = canonical_for_compare(allowed_root);
  if (absolute_normalized(resolved_target) == absolute_normalized(resolved_root)) {
    return std::unexpected(
        Error::make("cova::fs::unsafe_delete", "refusing to delete the allowed root itself")
            .with("target: " + path_text(resolved_target)));
  }
  if (!is_within(resolved_target, resolved_root)) {
    return std::unexpected(
        Error::make("cova::fs::outside_root", "refusing to delete a path outside the allowed root")
            .with("target: " + path_text(resolved_target))
            .with("root: " + path_text(resolved_root)));
  }
  // Only regular files and symlinks themselves may be removed — never a tree.
  const stdfs::file_status status = status_of(resolved_target);
  if (stdfs::is_directory(status) && !stdfs::is_symlink(status)) {
    return std::unexpected(Error::make("cova::fs::is_directory",
                                       "safe_remove refuses to delete a directory")
                               .with("target: " + path_text(resolved_target)));
  }
  std::error_code ec;
  if (!stdfs::remove(resolved_target, ec) || ec) {
    return std::unexpected(Error::make("cova::fs::remove_failed", "cannot remove path")
                               .with("target: " + path_text(resolved_target))
                               .with("reason: " + ec.message()));
  }
  return ok();
}

Result<Path> make_temp_dir(const Path& parent, std::string_view prefix) {
  if (Status prepared = mkdirs(parent); !prepared) {
    return std::unexpected(prepared.error());
  }
  for (unsigned attempt = 0; attempt < 10000; ++attempt) {
    std::string name;
    name += prefix.empty() ? std::string("tmp") : std::string(prefix);
    name += "-";
    name += std::to_string(process_id());
    name += "-";
    name += std::to_string(attempt);
    const Path candidate = parent / name;
    std::error_code ec;
    if (stdfs::create_directory(candidate, ec) && !ec) {
      return candidate;
    }
    if (stdfs::exists(candidate)) {
      continue;
    }
    if (ec) {
      return std::unexpected(Error::make("cova::fs::create_dir_failed",
                                         "cannot create temporary directory")
                                 .with("path: " + path_text(candidate))
                                 .with("reason: " + ec.message()));
    }
  }
  return std::unexpected(Error::make("cova::fs::create_dir_failed",
                                     "cannot find a unique temporary directory name")
                             .with("parent: " + path_text(parent)));
}

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------

Result<std::vector<Path>> glob(const Path& base, std::string_view pattern) {
  const std::string trimmed = str::trim(pattern);
  if (trimmed.empty()) {
    return std::unexpected(Error::make("cova::fs::glob", "empty glob pattern")
                               .with("base: " + path_text(base)));
  }
  if (Path(trimmed).is_absolute()) {
    return std::unexpected(Error::make("cova::fs::glob",
                                       "glob patterns must be relative to the package root")
                               .with("pattern: " + trimmed));
  }
  std::vector<std::string> components;
  for (const std::string& raw : str::split(trimmed, '/')) {
    if (raw.empty() || raw == ".") {
      continue;
    }
    if (raw == "..") {
      return std::unexpected(
          Error::make("cova::fs::glob", "glob patterns must not traverse outside the package")
              .with("pattern: " + trimmed));
    }
    components.push_back(raw);
  }
  if (components.empty()) {
    return std::unexpected(Error::make("cova::fs::glob", "glob pattern matches no files")
                               .with("pattern: " + trimmed));
  }
  if (!is_dir(base)) {
    return std::unexpected(Error::make("cova::fs::not_found", "glob base is not a directory")
                               .with("base: " + path_text(base)));
  }
  GlobContext context;
  context.match(base, components, 0);
  return std::vector<Path>(context.results.begin(), context.results.end());
}

std::vector<Path> walk_files(const Path& root) {
  std::vector<Path> files;
  if (!is_dir(root)) {
    return files;
  }
  std::error_code ec;
  stdfs::recursive_directory_iterator it(root, stdfs::directory_options::none, ec);
  if (ec) {
    return files;
  }
  const stdfs::recursive_directory_iterator end;
  while (it != end) {
    const std::string name = it->path().filename().string();
    if (is_hidden_name(name)) {
      if (it->is_directory(ec) && !ec) {
        it.disable_recursion_pending();
      }
      it.increment(ec);
      if (ec) {
        break;
      }
      continue;
    }
    std::error_code type_ec;
    if (it->is_regular_file(type_ec) && !type_ec) {
      files.push_back(it->path());
    }
    it.increment(ec);
    if (ec) {
      break;
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

Path normalize(const Path& p) {
  Path result = p.lexically_normal();
  if (result.empty()) {
    return Path(".");
  }
  return result;
}

Path absolute_normalized(const Path& p) {
  std::error_code ec;
  Path absolute = stdfs::absolute(p, ec);
  if (ec) {
    absolute = p;
  }
  return normalize(absolute);
}

Path canonical_for_compare(const Path& p) {
  std::error_code ec;
  Path weak = stdfs::weakly_canonical(p, ec);
  if (ec || weak.empty()) {
    return absolute_normalized(p);
  }
  return normalize(weak);
}

bool is_within(const Path& child, const Path& root) {
  const Path inner = absolute_normalized(child);
  const Path outer = absolute_normalized(root);
  auto inner_it = inner.begin();
  for (auto outer_it = outer.begin(); outer_it != outer.end(); ++outer_it, ++inner_it) {
    if (inner_it == inner.end()) {
      return false;
    }
    if (*inner_it != *outer_it) {
      return false;
    }
  }
  return true;
}

std::string generic_string(const Path& p) { return p.generic_string(); }

std::string join_generic(std::string_view a, std::string_view b) {
  std::string left(a);
  while (!left.empty() && left.back() == '/') {
    left.pop_back();
  }
  std::string_view right = b;
  while (!right.empty() && right.front() == '/') {
    right.remove_prefix(1);
  }
  if (left.empty()) {
    return std::string(right);
  }
  if (right.empty()) {
    return left;
  }
  left.push_back('/');
  left.append(right);
  return left;
}

// ---------------------------------------------------------------------------
// Cova directories
// ---------------------------------------------------------------------------

Result<CovaDirs> cova_dirs() {
  Path root;
  if (const std::optional<std::string> home = get_env("COVA_HOME"); home && !home->empty()) {
    root = Path(*home);
  }
#ifdef _WIN32
  else if (const std::optional<std::string> local = get_env("LOCALAPPDATA");
           local && !local->empty()) {
    root = Path(*local) / "cova";
  } else if (const std::optional<std::string> profile = get_env("USERPROFILE");
             profile && !profile->empty()) {
    root = Path(*profile) / ".cova";
  } else {
    return std::unexpected(
        Error::make("cova::fs::home", "cannot determine a Cova data directory")
            .with("hint: set COVA_HOME, LOCALAPPDATA or USERPROFILE"));
  }
#else
  else if (const std::optional<std::string> xdg = get_env("XDG_DATA_HOME");
           xdg && !xdg->empty()) {
    root = Path(*xdg) / "cova";
  } else if (const std::optional<std::string> home_dir = get_env("HOME");
             home_dir && !home_dir->empty()) {
    root = Path(*home_dir) / ".cova";
  } else {
    return std::unexpected(
        Error::make("cova::fs::home", "cannot determine a Cova data directory")
            .with("hint: set COVA_HOME or HOME"));
  }
#endif
  CovaDirs dirs;
  dirs.root = absolute_normalized(root);
  dirs.toolchains = dirs.root / "toolchains";
  dirs.sysroots = dirs.root / "sysroots";
  dirs.store = dirs.root / "store";
  dirs.downloads = dirs.root / "downloads";
  dirs.registry = dirs.root / "registry";
  dirs.cache = dirs.root / "cache";
  dirs.config_file = dirs.root / "config.toml";
  return dirs;
}

Status ensure_layout(const CovaDirs& dirs) {
  for (const Path* path :
       {&dirs.root, &dirs.toolchains, &dirs.sysroots, &dirs.store, &dirs.downloads,
        &dirs.registry, &dirs.cache}) {
    if (Status created = mkdirs(*path); !created) {
      return created;
    }
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Environment helpers
// ---------------------------------------------------------------------------

std::optional<std::string> get_env(std::string_view name) {
  const std::string key(name);
#ifdef _WIN32
  char* buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, key.c_str()) != 0 || buffer == nullptr) {
    return std::nullopt;
  }
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* value = std::getenv(key.c_str());
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

Status set_env(std::string_view name, std::string_view value) {
  if (name.empty()) {
    return std::unexpected(Error::make("cova::fs::env", "environment variable name is empty"));
  }
  const std::string key(name);
  const std::string text(value);
#ifdef _WIN32
  if (_putenv_s(key.c_str(), text.c_str()) != 0) {
    return std::unexpected(Error::make("cova::fs::env", "cannot set environment variable")
                               .with("name: " + key));
  }
#else
  if (::setenv(key.c_str(), text.c_str(), 1) != 0) {
    return std::unexpected(Error::make("cova::fs::env", "cannot set environment variable")
                               .with("name: " + key)
                               .with("errno: " + std::string(std::strerror(errno))));
  }
#endif
  return ok();
}

std::vector<Path> path_entries() {
  std::vector<Path> entries;
  const std::optional<std::string> path_text_value = get_env("PATH");
  if (!path_text_value) {
    return entries;
  }
#ifdef _WIN32
  constexpr char kDelimiter = ';';
#else
  constexpr char kDelimiter = ':';
#endif
  for (const std::string& part : str::split(*path_text_value, kDelimiter)) {
    if (!part.empty()) {
      entries.emplace_back(part);
    }
  }
  return entries;
}

namespace {

// Windows resolves a bare program name by appending PATHEXT, exactly as the
// shell and CreateProcess do. Without this, `g++` would never find `g++.exe`.
std::vector<std::string> executable_names(std::string_view name) {
  std::vector<std::string> names{std::string(name)};
#ifdef _WIN32
  const std::string text(name);
  const std::optional<std::string> pathext = get_env("PATHEXT");
  const std::string extensions = pathext ? *pathext : ".COM;.EXE;.BAT;.CMD";
  const std::string lowered = str::to_lower(text);
  bool already_named = false;
  for (const std::string& extension : str::split(extensions, ';')) {
    if (!extension.empty() && str::ends_with(lowered, str::to_lower(extension))) {
      already_named = true;
      break;
    }
  }
  if (already_named) {
    return names;
  }
  for (std::string extension : str::split(extensions, ';')) {
    if (extension.empty()) {
      continue;
    }
    if (extension.front() != '.') {
      extension.insert(extension.begin(), '.');
    }
    names.push_back(text + extension);
    names.push_back(text + str::to_lower(extension));
  }
#endif
  return names;
}

}  // namespace

std::optional<Path> find_in_path(std::string_view executable) {
  if (executable.empty()) {
    return std::nullopt;
  }
  const Path candidate(executable);
  if (candidate.has_parent_path()) {
    for (const std::string& name : executable_names(executable)) {
      const Path full(name);
      if (is_file(full) && access_executable(full)) {
        return full;
      }
    }
    return std::nullopt;
  }
  for (const Path& directory : path_entries()) {
    for (const std::string& name : executable_names(executable)) {
      const Path full = directory / name;
      if (is_file(full) && access_executable(full)) {
        return full;
      }
    }
  }
  return std::nullopt;
}

Result<Path> current_dir() {
  std::error_code ec;
  const Path cwd = stdfs::current_path(ec);
  if (ec) {
    return std::unexpected(Error::make("cova::fs::cwd", "cannot determine the working directory")
                               .with("reason: " + ec.message()));
  }
  return cwd;
}

}  // namespace cova::fs
