// Cova — formatter integration (PRD §11).
//
// Cova does not implement formatting. It resolves a clang-format binary with a
// resolution chain (system → managed → trusted download), selects files with the
// documented exclusions, and reports machine-readable results for CI.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"
#include "manifest/manifest.hpp"

namespace cova::formatter {

namespace stdfs = std::filesystem;

// Resolved formatter binary plus provenance for transparency.
struct FormatterDescriptor {
  stdfs::path path;
  std::string tool;          // "clang-format"
  std::string version_text;  // e.g. "clang-format version 20.1.0"
  std::string source;        // "system", "managed", "env"
  bool usable = false;
};

struct ResolveOptions {
  std::string tool = "clang-format";
  // Version requirement; formatter versioning is independent of the compiler
  // (PRD §11.2).
  std::string version;
  std::optional<stdfs::path> managed_root;
  bool allow_install = true;
  bool offline = false;
  // Honour $CLANG_FORMAT / $COVA_CLANG_FORMAT.
  bool use_env = true;
};

Result<FormatterDescriptor> resolve_formatter(const ResolveOptions& options);

// Which files a `cova fmt` run covers.
struct FileSetOptions {
  // Globs from [format].include, else the package source set.
  std::vector<std::string> include_globs;
  std::vector<std::string> exclude_globs;
  // Directories never formatted (build output, vendor, cache).
  std::vector<stdfs::path> excluded_roots;
};
Result<std::vector<stdfs::path>> collect_files(const stdfs::path& package_root,
                                               const manifest::Manifest& manifest,
                                               const FileSetOptions& options);

enum class FmtMode { Write, Check, Diff };

struct FmtRequest {
  FmtMode mode = FmtMode::Write;
  std::vector<stdfs::path> files;
  stdfs::path package_root;
  // "file" means use the nearest .clang-format; otherwise an inline style.
  std::string style = "file";
  std::string fallback_style = "LLVM";
  int jobs = 0;
  bool show_unchanged = false;
};

struct FileFormatResult {
  stdfs::path file;
  bool changed = false;      // would change (check) / did change (write)
  bool failed = false;
  std::string error;
  std::string diff_text;     // populated in Diff mode
};

struct FmtResult {
  bool success = true;
  std::vector<FileFormatResult> files;
  std::size_t changed = 0;
  std::size_t failed = 0;
  std::string to_json() const;
  std::string summary() const;
};

// Runs the formatter over the request.
Result<FmtResult> format_files(const FormatterDescriptor& formatter, const FmtRequest& request);

// Writes the default .clang-format used by `cova new` (PRD §5.1).
std::string default_clang_format_contents();

// True when `path` is a C/C++ source or header Cova formats by default.
bool is_formattable(const stdfs::path& path);

}  // namespace cova::formatter
