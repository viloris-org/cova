// Cova — git source fetching (PRD §10.3, §10.4).
#include "package/source/git.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/process/process.hpp"

namespace cova::package::git {
namespace {

using stdfs::path;

// A fetch or a checkout of a large repository legitimately takes minutes, but a
// dead remote or a stalled mount must never hang a build forever. Local object
// reads (rev-parse, ls-tree) are left untimed.
constexpr std::chrono::milliseconds kGitOperationTimeout{10 * 60 * 1000};

// Repository ids are derived from the credential-free URL so two remotes never
// share a cache directory, the same repository reached with two different
// tokens is cloned once, and no token ends up in a cache path.
std::string repository_id(std::string_view url) {
  const std::string hex = to_hex(Sha256::of(redact_url(url)));
  return hex.substr(0, 32);
}

std::optional<path> find_git() {
  for (std::string_view name : {"git", "git.exe"}) {
    if (auto found = fs::find_in_path(name)) {
      return *found;
    }
  }
  return std::nullopt;
}

Error git_error(std::string code, std::string message, const proc::ProcessResult& result) {
  Error error = Error::make(std::move(code), std::move(message));
  if (result.timed_out) {
    error.with("git did not finish before the timeout");
  }
  if (!result.stderr_text.empty()) {
    error.with("git stderr: " + result.stderr_trimmed());
  }
  return error;
}

// Runs git with the given arguments. `args` never goes through a shell, so URLs
// and paths containing spaces or metacharacters are safe by construction.
Result<proc::ProcessResult> run_git(
    const path& git, std::vector<std::string> args,
    std::vector<std::pair<std::string, std::string>> env = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
  proc::ProcessSpec spec;
  spec.program = git;
  spec.args = std::move(args);
  spec.env_overrides = std::move(env);
  spec.timeout = timeout;
  return proc::run(spec);
}

// Resolves a candidate revision to a commit id without touching the network.
std::optional<std::string> resolve_cached_commit(const path& git, const path& db,
                                                 std::string_view rev) {
  std::vector<std::string> candidates;
  if (rev.empty()) {
    // `refs/cova/default` is what the last fetch recorded from the remote's own
    // HEAD, so a repository whose default branch is neither main nor master
    // still resolves without guessing.
    candidates = {"refs/cova/default", "refs/heads/main", "refs/heads/master", "HEAD"};
  } else {
    const std::string text(rev);
    // A tag wins over a branch of the same name, then the remote-tracking ref,
    // then the bare name. The order is fixed so a repository that uses both
    // spellings resolves the same way on every machine.
    candidates = {"refs/tags/" + text, "refs/heads/" + text, "refs/remotes/origin/" + text, text};
  }

  for (const auto& candidate : candidates) {
    auto result = run_git(git, {"--git-dir=" + fs::generic_string(db), "rev-parse", "--verify",
                                "--quiet", candidate + "^{commit}"});
    if (!result) {
      continue;
    }
    if (result->success()) {
      std::string commit = result->stdout_trimmed();
      if (commit.size() == 40) {
        return commit;
      }
    }
  }

  if (rev.empty()) {
    // No conventional default branch and no recorded HEAD: fall back to the only
    // branch present.
    auto listed = run_git(git, {"--git-dir=" + fs::generic_string(db), "for-each-ref",
                                "--format=%(refname)", "refs/heads"});
    if (listed && listed->success()) {
      std::vector<std::string> branches = str::split(listed->stdout_trimmed(), '\n');
      std::vector<std::string> kept;
      for (auto& branch : branches) {
        // Trim so a CRLF-producing git build does not corrupt the ref name.
        branch = str::trim(branch);
        if (!branch.empty()) {
          kept.push_back(std::move(branch));
        }
      }
      if (kept.size() == 1) {
        auto resolved = run_git(git, {"--git-dir=" + fs::generic_string(db), "rev-parse",
                                      "--verify", "--quiet", kept.front() + "^{commit}"});
        if (resolved && resolved->success()) {
          std::string commit = resolved->stdout_trimmed();
          if (commit.size() == 40) {
            return commit;
          }
        }
      }
    }
  }
  return std::nullopt;
}

Status ensure_bare_repository(const path& git, const path& db, const std::string& url) {
  const bool initialized = fs::is_directory(db) && fs::is_regular_file(db / "HEAD");
  if (!initialized) {
    auto created = fs::create_directories(db);
    if (!created) {
      return created;
    }
    auto init = run_git(git, {"init", "--bare", "--quiet", fs::generic_string(db)});
    if (!init) {
      return fail(init.error());
    }
    if (!init->success() && !(fs::is_directory(db) && fs::is_regular_file(db / "HEAD"))) {
      return fail(git_error("cova::package::git_init_failed",
                            "could not initialise the git source cache", *init));
    }
  }

  // Always (re)assert the remote instead of only doing it right after `init`.
  // That is idempotent under concurrent fetches, and it repairs a cache entry
  // left behind by a run killed between `git init` and `git remote add`, which
  // would otherwise stay unusable until the user deleted the directory.
  auto current = run_git(git, {"--git-dir=" + fs::generic_string(db), "config", "--get",
                               "remote.origin.url"});
  if (!current) {
    return fail(current.error());
  }
  if (current->success() && str::trim(current->stdout_text) == url) {
    return ok();
  }
  auto configured = run_git(git, {"--git-dir=" + fs::generic_string(db), "config",
                                  "remote.origin.url", url});
  if (!configured) {
    return fail(configured.error());
  }
  if (!configured->success()) {
    return fail(git_error("cova::package::git_remote_failed",
                          "could not record the git remote `" + redact_url(url) + "`",
                          *configured));
  }
  return ok();
}

// Hashes the commit's tree exactly as git stores it: sorted path, mode and
// object id. Blob ids are content addresses, so this is a content hash of the
// source while staying independent of how the working tree was materialised.
Result<std::string> hash_commit_tree(const path& git, const path& db, std::string_view commit) {
  auto listed = run_git(git, {"--git-dir=" + fs::generic_string(db), "ls-tree", "-r", "-z",
                              "--full-tree", std::string(commit)});
  if (!listed) {
    return fail<std::string>(listed.error());
  }
  if (!listed->success()) {
    return fail<std::string>(
        git_error("cova::package::source_hash_failed",
                  "could not read the tree of revision " + std::string(commit), *listed));
  }
  Fingerprint builder;
  // `-z` keeps the listing unquoted, so `core.quotePath` cannot change it.
  builder.add_bytes(std::as_bytes(std::span<const char>(listed->stdout_text.data(),
                                                        listed->stdout_text.size())));
  return builder.finish_hex();
}

// Removes a staging directory unless publication moved it out of the way. A
// fetch that fails halfway must not leave `tmp/checkout-*` behind forever.
class StagingGuard {
 public:
  StagingGuard(path directory, path allowed_root)
      : directory_(std::move(directory)), allowed_root_(std::move(allowed_root)) {}
  ~StagingGuard() {
    if (!directory_.empty()) {
      (void)fs::remove_tree(directory_, allowed_root_);
    }
  }
  StagingGuard(const StagingGuard&) = delete;
  StagingGuard& operator=(const StagingGuard&) = delete;

 private:
  path directory_;
  path allowed_root_;
};

}  // namespace

bool is_full_commit(std::string_view rev) {
  if (rev.size() != 40) {
    return false;
  }
  for (char c : rev) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}

std::string redact_url(std::string_view url) {
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    return std::string(url);
  }
  const std::size_t authority = scheme + 3;
  const auto at = url.find('@', authority);
  if (at == std::string_view::npos) {
    return std::string(url);
  }
  // An `@` after the first path separator belongs to the path, not the user.
  const auto slash = url.find('/', authority);
  if (slash != std::string_view::npos && slash < at) {
    return std::string(url);
  }
  if (url.substr(authority, at - authority) == "git") {
    return std::string(url);
  }
  std::string out(url.substr(0, authority));
  out += "***@";
  out.append(url.substr(at + 1));
  return out;
}

Result<FetchedSource> fetch(const FetchRequest& request) {
  if (request.url.empty()) {
    return fail<FetchedSource>(
        Error::make("cova::package::git_url_missing", "a git dependency needs a URL"));
  }
  if (request.url.front() == '-') {
    // A URL that looks like an option would be parsed as one by git.
    return fail<FetchedSource>(
        Error::make("cova::package::git_url_invalid",
                    "a git URL may not start with `-`")
            .with("url: " + redact_url(request.url)));
  }
  if (request.cache_root.empty() || request.cache_root.is_relative()) {
    // A relative cache root would resolve against the process working directory
    // and scatter bare repositories through the project tree.
    return fail<FetchedSource>(
        Error::make("cova::package::git_cache_invalid",
                    "the git source cache path must be absolute")
            .with("cache: " + fs::generic_string(request.cache_root)));
  }

  auto git = find_git();
  if (!git) {
    return fail<FetchedSource>(
        Error::make("cova::package::git_missing",
                    "`git` was not found on PATH, so git dependencies cannot be fetched")
            .with("url: " + redact_url(request.url))
            .with("install git, or vendor the dependency and use a `path` dependency"));
  }

  const path cache_root = request.cache_root;
  const std::string id = repository_id(request.url);
  const path db = cache_root / "db" / id;
  const path checkout_root = cache_root / "checkouts" / id;

  auto layout = fs::create_directories(cache_root / "tmp");
  if (!layout) {
    return fail<FetchedSource>(layout.error());
  }

  bool downloaded = false;
  const bool had_repository = fs::is_regular_file(db / "HEAD");
  if (!had_repository && request.offline) {
    return fail<FetchedSource>(
        Error::make("cova::package::source_not_cached",
                    "git dependency `" + redact_url(request.url) +
                        "` is not in the local cache and offline mode forbids fetching it")
            .with("cache: " + fs::generic_string(cache_root))
            .with("run once without --offline to populate the cache"));
  }
  // Always run the (purely local) repository preparation: it creates the cache
  // entry when missing and repairs one whose `origin` was lost, which otherwise
  // could never fetch again.
  auto ensured = ensure_bare_repository(*git, db, request.url);
  if (!ensured) {
    return fail<FetchedSource>(ensured.error());
  }

  std::optional<std::string> commit;
  if (!request.refresh) {
    commit = resolve_cached_commit(*git, db, request.rev);
  }
  if (!commit) {
    if (request.offline) {
      const std::string what = request.rev.empty() ? "the default branch" : "`" + request.rev + "`";
      return fail<FetchedSource>(
          Error::make("cova::package::source_not_cached",
                      "git dependency `" + redact_url(request.url) + "` at " + what +
                          " is not in the local cache and offline mode forbids fetching it")
              .with("cache: " + fs::generic_string(cache_root))
              .with("run once without --offline to populate the cache"));
    }
    // Fetch heads and tags into real refs rather than remote-tracking refs: the
    // cache must be able to answer a tag or branch lookup without the network.
    // The remote's HEAD is recorded under a Cova-owned ref so `rev`-less
    // dependencies resolve the actual default branch offline, even when it is
    // called something other than main or master.
    std::vector<std::string> fetch_args = {"--git-dir=" + fs::generic_string(db),
                                           "fetch",
                                           "--quiet",
                                           "--force",
                                           "--prune",
                                           "origin",
                                           "+refs/heads/*:refs/heads/*",
                                           "+refs/tags/*:refs/tags/*"};
    if (request.rev.empty()) {
      fetch_args.emplace_back("+HEAD:refs/cova/default");
    }
    auto fetched = run_git(*git, std::move(fetch_args), {}, kGitOperationTimeout);
    if (!fetched) {
      return fail<FetchedSource>(fetched.error());
    }
    if (!fetched->success()) {
      return fail<FetchedSource>(git_error(
          "cova::package::git_fetch_failed",
          "could not fetch git dependency `" + redact_url(request.url) + "`", *fetched));
    }
    downloaded = true;
    commit = resolve_cached_commit(*git, db, request.rev);
    if (!commit && is_full_commit(request.rev)) {
      // A locked commit can be unreachable from any branch (the branch was
      // deleted or rewound). Ask the server for that object explicitly before
      // giving up, so a lockfile stays usable when the server allows it.
      auto specific = run_git(*git, {"--git-dir=" + fs::generic_string(db), "fetch", "--quiet",
                                     "origin", request.rev},
                              {}, kGitOperationTimeout);
      if (specific && specific->success()) {
        commit = resolve_cached_commit(*git, db, request.rev);
      }
    }
    if (!commit) {
      if (request.rev.empty()) {
        return fail<FetchedSource>(
            Error::make("cova::package::source_rev_unknown",
                        "git dependency `" + redact_url(request.url) +
                            "` has no default branch Cova can identify")
                .with("the remote did not report a usable HEAD")
                .with("declare `rev = \"<tag or branch>\"` in the dependency"));
      }
      Error error = Error::make(
          "cova::package::source_rev_unknown",
          "git dependency `" + redact_url(request.url) + "` has no revision matching `" +
              request.rev + "`");
      error.with("checked refs/tags, refs/heads and the bare revision name");
      error.with("declare a tag, a branch, or a full 40-character commit id in `rev`");
      return fail<FetchedSource>(std::move(error));
    }
  }

  const path checkout = checkout_root / *commit;
  auto status = fs::create_directories(checkout_root);
  if (!status) {
    return fail<FetchedSource>(status.error());
  }

  // The hash comes from the object store, so it is computed the same way whether
  // the checkout already exists or is about to be created.
  auto tree_hash = hash_commit_tree(*git, db, *commit);
  if (!tree_hash) {
    return fail<FetchedSource>(tree_hash.error());
  }

  if (fs::is_directory(checkout)) {
    // A cached checkout is reused as-is: git verified every blob against its
    // object id while writing it, and the tree hash identifies the content.
    return FetchedSource{checkout, *commit, *tree_hash, downloaded};
  }

  // Extract into a staging directory and publish it with an atomic rename, so
  // an interrupted fetch can never leave a half-populated package root.
  auto staging = fs::make_temp_dir(cache_root / "tmp", "checkout-");
  if (!staging) {
    return fail<FetchedSource>(staging.error());
  }
  StagingGuard cleanup{*staging, cache_root};
  const path tree = *staging / "tree";
  auto made = fs::create_directories(tree);
  if (!made) {
    return fail<FetchedSource>(made.error());
  }
  proc::ProcessSpec checkout_spec;
  checkout_spec.program = *git;
  checkout_spec.args = {"--git-dir=" + fs::generic_string(db),
                        "--work-tree=" + fs::generic_string(tree), "checkout", "--force", *commit,
                        "--", "."};
  checkout_spec.env_overrides = {{"GIT_INDEX_FILE", fs::generic_string(*staging / "index")}};
  checkout_spec.timeout = kGitOperationTimeout;
  auto checked_out = proc::run(checkout_spec);
  if (!checked_out) {
    return fail<FetchedSource>(checked_out.error());
  }
  if (!checked_out->success()) {
    return fail<FetchedSource>(
        git_error("cova::package::git_checkout_failed",
                  "could not check out revision " + *commit + " of `" + redact_url(request.url) +
                      "`",
                  *checked_out));
  }

  std::error_code ec;
  stdfs::rename(tree, checkout, ec);
  if (ec && !fs::is_directory(checkout)) {
    return fail<FetchedSource>(
        Error::make("cova::package::git_checkout_failed",
                    "could not publish the checked out source tree")
            .with("from: " + fs::generic_string(tree))
            .with("to: " + fs::generic_string(checkout))
            .with("error: " + ec.message()));
  }
  // When the rename failed because another process published the same commit
  // first, `checkout` is already the complete tree and is used as-is.
  return FetchedSource{checkout, *commit, *tree_hash, downloaded};
}

}  // namespace cova::package::git
