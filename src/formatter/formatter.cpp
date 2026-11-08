// Cova — formatter integration (PRD §11).
#include "formatter/formatter.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "core/process/process.hpp"
#include "core/version/version.hpp"

namespace cova::formatter {
namespace {

bool has_glob_magic(std::string_view pattern) {
  return pattern.find('*') != std::string_view::npos ||
         pattern.find('?') != std::string_view::npos ||
         pattern.find('[') != std::string_view::npos;
}

bool excluded_by_glob(const stdfs::path& file, const stdfs::path& root,
                      const std::vector<std::string>& globs) {
  const std::string relative = fs::generic_string(file.lexically_relative(root));
  for (const auto& pattern : globs) {
    if (pattern == relative || str::ends_with(relative, "/" + pattern)) {
      return true;
    }
    if (has_glob_magic(pattern)) {
      auto matches = fs::glob(root, pattern);
      if (matches) {
        for (const auto& match : *matches) {
          if (fs::canonical_for_compare(match) == fs::canonical_for_compare(file)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool under_any(const stdfs::path& file, const std::vector<stdfs::path>& roots) {
  for (const auto& root : roots) {
    if (!root.empty() && fs::is_within(file, root)) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> style_arguments(const FmtRequest& request) {
  std::vector<std::string> args;
  const std::string style = request.style.empty() ? "file" : request.style;
  if (style == "file" || style == "File") {
    args.push_back("-style=file");
  } else {
    args.push_back("-style=" + style);
  }
  if (!request.fallback_style.empty()) {
    args.push_back("-fallback-style=" + request.fallback_style);
  }
  return args;
}

Result<proc::ProcessResult> run_formatter(const FormatterDescriptor& formatter,
                                          const std::vector<std::string>& args,
                                          const stdfs::path& cwd) {
  proc::ProcessSpec spec;
  spec.program = formatter.path;
  spec.args = args;
  spec.cwd = cwd;
  spec.timeout = std::chrono::milliseconds(120000);
  return proc::run(spec);
}

std::string first_line(std::string_view text) {
  auto newline = text.find('\n');
  std::string line = std::string(text.substr(0, newline));
  return str::trim(line);
}

}  // namespace

Result<FormatterDescriptor> resolve_formatter(const ResolveOptions& options) {
  std::vector<std::pair<stdfs::path, std::string>> candidates;
  if (options.use_env) {
    for (const std::string_view variable : {"COVA_CLANG_FORMAT", "CLANG_FORMAT"}) {
      if (auto value = fs::get_env(variable); value && !value->empty()) {
        candidates.emplace_back(*value, "env");
      }
    }
  }
  if (options.managed_root) {
    for (const auto& name : {"clang-format", "clang-format.exe"}) {
      const auto candidate = *options.managed_root / "clang-format" / "bin" / name;
      if (fs::is_regular_file(candidate)) {
        candidates.emplace_back(candidate, "managed");
      }
    }
  }
  for (const auto& directory : fs::path_entries()) {
    const auto candidate = directory / options.tool;
    if (fs::is_regular_file(candidate)) {
      candidates.emplace_back(candidate, "system");
    }
  }

  if (candidates.empty()) {
    Error error = Error::make("cova::format::not_found",
                              "`" + options.tool + "` was not found on PATH");
    if (options.offline || !options.allow_install) {
      error.with("installation is disabled in this invocation, so Cova cannot fetch it");
    } else {
      error.with("install clang-format, or set $COVA_CLANG_FORMAT to its path");
    }
    return fail<FormatterDescriptor>(std::move(error));
  }

  for (const auto& [path, source] : candidates) {
    proc::ProcessSpec spec;
    spec.program = path;
    spec.args = {"--version"};
    spec.timeout = std::chrono::milliseconds(5000);
    auto result = proc::run(spec);
    if (!result || !result->success()) {
      continue;
    }
    FormatterDescriptor descriptor;
    descriptor.path = path;
    descriptor.tool = options.tool;
    descriptor.version_text = first_line(result->stdout_text.empty() ? result->stderr_text
                                                                     : result->stdout_text);
    descriptor.source = source;
    descriptor.usable = true;
    if (!options.version.empty()) {
      auto requirement = VersionRequirement::parse(options.version);
      auto version = Version::extract(descriptor.version_text);
      if (requirement && version && !requirement->matches(*version)) {
        continue;
      }
    }
    return descriptor;
  }
  return fail<FormatterDescriptor>(Error::make(
      "cova::format::unusable",
      "`" + options.tool + "` was found but could not be executed"));
}

Result<std::vector<stdfs::path>> collect_files(const stdfs::path& package_root,
                                               const manifest::Manifest& manifest,
                                               const FileSetOptions& options) {
  std::set<std::string> seen;
  std::vector<stdfs::path> files;

  auto add = [&](const stdfs::path& file) {
    if (!is_formattable(file)) {
      return;
    }
    if (under_any(file, options.excluded_roots)) {
      return;
    }
    if (excluded_by_glob(file, package_root, options.exclude_globs)) {
      return;
    }
    const std::string key = fs::generic_string(fs::canonical_for_compare(file));
    if (seen.insert(key).second) {
      files.push_back(file);
    }
  };

  auto expand = [&](const std::string& pattern) {
    if (!has_glob_magic(pattern)) {
      const auto path = package_root / pattern;
      if (fs::is_directory(path)) {
        for (const auto& file : fs::walk_files(path)) {
          add(file);
        }
      } else if (fs::is_regular_file(path)) {
        add(path);
      }
      return;
    }
    auto matches = fs::glob(package_root, pattern);
    if (!matches) {
      return;
    }
    for (const auto& match : *matches) {
      if (fs::is_directory(match)) {
        for (const auto& file : fs::walk_files(match)) {
          add(file);
        }
      } else {
        add(match);
      }
    }
  };

  if (!options.include_globs.empty()) {
    for (const auto& pattern : options.include_globs) {
      expand(pattern);
    }
  } else {
    for (const auto& pattern : manifest.build.sources) {
      expand(pattern);
    }
    for (const auto& pattern : manifest.build.source_dirs) {
      expand(pattern);
    }
    for (const auto& directory : manifest.build.public_includes) {
      expand(directory);
    }
    for (const auto& directory : manifest.build.private_includes) {
      expand(directory);
    }
    for (const auto& pattern : manifest.test.sources) {
      expand(pattern);
    }
  }

  std::sort(files.begin(), files.end());
  return files;
}

Result<FmtResult> format_files(const FormatterDescriptor& formatter, const FmtRequest& request) {
  FmtResult result;
  const auto style = style_arguments(request);
  for (const auto& file : request.files) {
    FileFormatResult entry;
    entry.file = file;
    if (!fs::is_regular_file(file)) {
      entry.failed = true;
      entry.error = "file does not exist";
      result.failed += 1;
      result.success = false;
      result.files.push_back(std::move(entry));
      continue;
    }

    std::vector<std::string> args = style;
    if (request.mode == FmtMode::Write) {
      args.push_back("-i");
      args.push_back(fs::generic_string(file));
      auto run = run_formatter(formatter, args, request.package_root);
      if (!run || !run->success()) {
        entry.failed = true;
        entry.error = run ? str::trim(run->stderr_text) : run.error().message;
        result.failed += 1;
        result.success = false;
      }
    } else if (request.mode == FmtMode::Check) {
      args.push_back("--dry-run");
      args.push_back("--Werror");
      args.push_back(fs::generic_string(file));
      auto run = run_formatter(formatter, args, request.package_root);
      if (!run) {
        entry.failed = true;
        entry.error = run.error().message;
        result.failed += 1;
        result.success = false;
      } else if (run->success()) {
        entry.changed = false;
      } else if (run->exit_code == 1) {
        entry.changed = true;
        result.changed += 1;
        result.success = false;
      } else {
        entry.failed = true;
        entry.error = str::trim(run->stderr_text);
        result.failed += 1;
        result.success = false;
      }
    } else {
      // Diff mode: format to stdout and compare with the current contents.
      args.push_back(fs::generic_string(file));
      auto run = run_formatter(formatter, args, request.package_root);
      if (!run || !run->success()) {
        entry.failed = true;
        entry.error = run ? str::trim(run->stderr_text) : run.error().message;
        result.failed += 1;
        result.success = false;
      } else {
        auto current = fs::read_file(file);
        if (!current) {
          entry.failed = true;
          entry.error = current.error().message;
          result.failed += 1;
          result.success = false;
        } else if (*current != run->stdout_text) {
          entry.changed = true;
          result.changed += 1;
          entry.diff_text = "--- " + fs::generic_string(file) + "\n+++ " +
                            fs::generic_string(file) + " (formatted)\n";
        }
      }
    }
    result.files.push_back(std::move(entry));
  }
  return result;
}

std::string FmtResult::to_json() const {
  json::Value root = json::Value::object();
  root.set("success", success);
  root.set("changed", static_cast<std::int64_t>(changed));
  root.set("failed", static_cast<std::int64_t>(failed));
  auto array = json::Value::array();
  for (const auto& file : files) {
    json::Value entry = json::Value::object();
    entry.set("file", fs::generic_string(file.file));
    entry.set("changed", file.changed);
    entry.set("failed", file.failed);
    if (!file.error.empty()) {
      entry.set("error", file.error);
    }
    array.push(std::move(entry));
  }
  root.set("files", std::move(array));
  return root.dump(2);
}

std::string FmtResult::summary() const {
  return std::to_string(files.size()) + " file(s), " + std::to_string(changed) + " changed, " +
         std::to_string(failed) + " failed";
}

std::string default_clang_format_contents() {
  return R"(---
BasedOnStyle: LLVM
Language: Cpp
IndentWidth: 2
ColumnLimit: 100
AccessModifierOffset: -2
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: Never
BreakBeforeBraces: Attach
PointerAlignment: Left
SortIncludes: CaseSensitive
SpaceAfterTemplateKeyword: false
Standard: c++26
...
)";
}

bool is_formattable(const stdfs::path& path) {
  const std::string extension = str::to_lower(path.extension().string());
  static const std::vector<std::string> extensions = {
      ".c",   ".cc",  ".cpp", ".cxx", ".c++", ".h",  ".hh",  ".hpp",
      ".hxx", ".h++", ".inl", ".ipp", ".tpp", ".ixx", ".cppm", ".m"};
  return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

}  // namespace cova::formatter
