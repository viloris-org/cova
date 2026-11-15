// Cova — cached compiler queries (PRD §7.2, §19.3).
//
// Discovery asks a driver a handful of questions that are pure functions of the
// driver binary: `--version`, `-print-resource-dir`, `--print-sysroot`,
// `-print-prog-name=…`. Each answer costs a process, and a driver like clang
// takes ~10ms just to start, so a warm build used to spend most of its
// toolchain resolution budget re-asking questions whose answers cannot have
// changed. The answers are stored beside the probe cache, keyed by the driver's
// identity *and* its metadata sample: replacing the compiler invalidates them
// automatically, and a stale file is never authoritative.
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "core/common.hpp"
#include "core/process/process.hpp"

namespace cova::toolchain {

namespace stdfs = std::filesystem;

// Runs `program args` and returns its captured output. With a non-empty
// `cache_root`, a successful answer is reused while `program` keeps the same
// path, inode, size and modification time.
Result<proc::ProcessResult> run_query(const stdfs::path& program,
                                      const std::vector<std::string>& args,
                                      const stdfs::path& cache_root,
                                      std::chrono::milliseconds timeout);

// SHA-256 of a toolchain component as lowercase hex, or "" when unreadable.
// Caching it keeps a build from re-reading every compiler, linker and archiver
// binary just to prove they did not change: the stored digest is reused while
// the file's metadata sample is unchanged, and recomputed otherwise.
std::string component_digest(const stdfs::path& path, const stdfs::path& cache_root);

}  // namespace cova::toolchain
