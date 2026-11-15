// Cova — cached compiler queries (implementation).
#include "toolchain/cache/query_cache.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"

namespace cova::toolchain {
namespace {

// Bumping the schema discards every stored answer; the key embeds it, so old
// files simply stop being addressed.
constexpr std::string_view kQueryCacheSchema = "cova.toolchain-query/1";
constexpr std::string_view kQueryCachePrefix = "cova-query/1";

std::string query_key(const stdfs::path& program, const std::vector<std::string>& args) {
  const fs::FileStamp stamp = fs::stamp(program);
  Fingerprint fingerprint;
  fingerprint.add(kQueryCacheSchema);
  fingerprint.add(fs::generic_string(fs::canonical_for_compare(program)));
  fingerprint.add(stamp.mtime_ns);
  fingerprint.add(static_cast<std::int64_t>(stamp.size));
  fingerprint.add(static_cast<std::int64_t>(stamp.inode));
  for (const auto& argument : args) {
    fingerprint.add(argument);
  }
  return fingerprint.finish_hex();
}

// A tiny length-prefixed record: compiler output is arbitrary bytes, and TOML
// string escaping would pay per byte for no benefit here.
std::string encode(const proc::ProcessResult& result) {
  std::string encoded(kQueryCachePrefix);
  encoded.push_back('\n');
  encoded += "exit " + std::to_string(result.exit_code) + "\n";
  encoded += "stdout " + std::to_string(result.stdout_text.size()) + "\n";
  encoded += result.stdout_text;
  encoded += "\nstderr " + std::to_string(result.stderr_text.size()) + "\n";
  encoded += result.stderr_text;
  return encoded;
}

bool decode(std::string_view text, proc::ProcessResult& result) {
  if (!text.starts_with(kQueryCachePrefix)) {
    return false;
  }
  std::size_t position = kQueryCachePrefix.size() + 1;

  auto number = [&](std::string_view name, long long& out) -> bool {
    const std::string prefix = std::string(name) + " ";
    if (text.substr(position, prefix.size()) != prefix) {
      return false;
    }
    position += prefix.size();
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string_view::npos) {
      return false;
    }
    const std::string_view digits = text.substr(position, newline - position);
    position = newline + 1;
    if (digits.empty()) {
      return false;
    }
    long long value = 0;
    for (const char character : digits) {
      if (character < '0' || character > '9') {
        return false;
      }
      value = value * 10 + (character - '0');
    }
    out = value;
    return true;
  };

  long long exit_code = 0;
  long long stdout_bytes = 0;
  long long stderr_bytes = 0;
  if (!number("exit", exit_code) || !number("stdout", stdout_bytes)) {
    return false;
  }
  if (static_cast<std::size_t>(stdout_bytes) + position + 1 > text.size()) {
    return false;
  }
  result.stdout_text.assign(text.substr(position, static_cast<std::size_t>(stdout_bytes)));
  position += static_cast<std::size_t>(stdout_bytes);
  if (text[position] != '\n') {
    return false;
  }
  ++position;
  if (!number("stderr", stderr_bytes)) {
    return false;
  }
  if (static_cast<std::size_t>(stderr_bytes) + position > text.size()) {
    return false;
  }
  result.stderr_text.assign(text.substr(position, static_cast<std::size_t>(stderr_bytes)));
  result.exit_code = static_cast<int>(exit_code);
  return true;
}

struct DigestEntry {
  fs::FileStamp stamp;
  std::string digest;
};

// One process keeps a memo so a shared linker is hashed once, not once per
// candidate; the file beside the probe cache carries it across builds.
std::map<std::string, DigestEntry>& digest_memo() {
  static std::map<std::string, DigestEntry> memo;
  return memo;
}

std::mutex& digest_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::string digest_path(const stdfs::path& cache_root, const stdfs::path& path) {
  const std::string key = sha256_hex(fs::generic_string(fs::canonical_for_compare(path)));
  return fs::generic_string(cache_root / "toolchain" / "digests" / (key + ".txt"));
}

}  // namespace

std::string component_digest(const stdfs::path& path, const stdfs::path& cache_root) {
  const std::string key = fs::generic_string(path);
  const fs::FileStamp stamp = fs::stamp(path);
  {
    std::lock_guard<std::mutex> lock(digest_mutex());
    auto& memo = digest_memo();
    if (auto it = memo.find(key); it != memo.end() && it->second.stamp == stamp) {
      return it->second.digest;
    }
  }
  std::string digest;
  if (!cache_root.empty()) {
    const stdfs::path cached_path(digest_path(cache_root, path));
    if (fs::is_regular_file(cached_path)) {
      if (auto text = fs::read_file(cached_path)) {
        // "<mtime> <size> <inode>\n<digest>"
        const std::size_t newline = text->find('\n');
        if (newline != std::string::npos) {
          long long mtime = 0;
          unsigned long long size = 0;
          unsigned long long inode = 0;
          if (std::sscanf(text->c_str(), "%lld %llu %llu", &mtime, &size, &inode) == 3 &&
              mtime == stamp.mtime_ns && size == stamp.size && inode == stamp.inode &&
              stamp.exists) {
            digest = text->substr(newline + 1);
            while (!digest.empty() && (digest.back() == '\n' || digest.back() == '\r')) {
              digest.pop_back();
            }
          }
        }
      }
    }
  }
  if (digest.empty()) {
    if (!stamp.exists) {
      return {};
    }
    digest = Sha256::hex_of_file(path).value_or("");
    if (digest.empty()) {
      return {};
    }
    if (!cache_root.empty()) {
      const stdfs::path cached_path(digest_path(cache_root, path));
      if (fs::create_directories(cached_path.parent_path())) {
        const std::string encoded = std::to_string(stamp.mtime_ns) + " " +
                                    std::to_string(stamp.size) + " " +
                                    std::to_string(stamp.inode) + "\n" + digest + "\n";
        (void)fs::write_file_atomic(cached_path, encoded);
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(digest_mutex());
    digest_memo()[key] = DigestEntry{stamp, digest};
  }
  return digest;
}

Result<proc::ProcessResult> run_query(const stdfs::path& program,
                                      const std::vector<std::string>& args,
                                      const stdfs::path& cache_root,
                                      std::chrono::milliseconds timeout) {
  stdfs::path cache_file;
  if (!cache_root.empty()) {
    cache_file = cache_root / "toolchain" / "queries" / (query_key(program, args) + ".txt");
    if (fs::is_regular_file(cache_file)) {
      if (auto text = fs::read_file(cache_file)) {
        proc::ProcessResult cached;
        if (decode(*text, cached)) {
          return cached;
        }
      }
    }
  }

  proc::ProcessSpec spec;
  spec.program = program;
  spec.args = args;
  spec.timeout = timeout;
  auto result = proc::run(spec);
  if (!result) {
    // The process could not be started at all: not an answer, so not stored.
    return result;
  }
  // A run that produced an exit code is an answer, including a rejection such
  // as GCC's "unrecognized option" for a clang-only query. Caching it is what
  // stops every build from paying for the same stack of failed process starts.
  if (!cache_file.empty()) {
    if (auto created = fs::create_directories(cache_file.parent_path())) {
      (void)fs::write_file_atomic(cache_file, encode(*result));
    }
  }
  return result;
}

}  // namespace cova::toolchain
