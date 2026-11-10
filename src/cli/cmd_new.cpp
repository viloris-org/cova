// Cova — `cova new` and `cova init` (PRD §5.1, §5.2).
//
// Generating a project is the first thing a user ever sees, so the templates are
// deliberately minimal and the generated files must compile immediately.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "cli/common.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "formatter/formatter.hpp"
#include "manifest/manifest.hpp"

namespace cova::cli {
namespace {

// Directory name → package name: lowercase, '-' for invalid characters.
std::string sanitise_package_name(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0) {
      out.push_back(static_cast<char>(std::tolower(uc)));
    } else if (c == '-' || c == '_' || c == ' ' || c == '.') {
      out.push_back('-');
    }
  }
  // Collapse duplicate separators and trim them from both ends.
  std::string collapsed;
  for (char c : out) {
    if (c == '-' && !collapsed.empty() && collapsed.back() == '-') {
      continue;
    }
    collapsed.push_back(c);
  }
  while (!collapsed.empty() && collapsed.front() == '-') {
    collapsed.erase(collapsed.begin());
  }
  while (!collapsed.empty() && collapsed.back() == '-') {
    collapsed.pop_back();
  }
  if (collapsed.empty() || !std::isalpha(static_cast<unsigned char>(collapsed.front()))) {
    collapsed.insert(collapsed.begin(), 'p');
  }
  return collapsed;
}

std::string executable_main() {
  return R"(#include <print>

int main(int argc, char** argv) {
  // Replace this with your program. `std::println` is C++23, available in the
  // C++26 baseline Cova targets.
  std::println("Hello from {}", argc > 0 ? argv[0] : "cova");
  return 0;
}
)";
}

std::string library_header(const std::string& name) {
  std::string out;
  out += "#pragma once\n\n";
  out += "namespace " + str::replace_all(name, "-", "_") + " {\n\n";
  out += "// Replace with your library's interface.\n";
  out += "int answer();\n\n";
  out += "}  // namespace " + str::replace_all(name, "-", "_") + "\n";
  return out;
}

std::string library_source(const std::string& name) {
  return "#include \"" + name + ".hpp\"\n\nnamespace " +
         str::replace_all(name, "-", "_") + " {\n\nint answer() { return 42; }\n\n}  // namespace " +
         str::replace_all(name, "-", "_") + "\n";
}

std::string test_main() {
  return R"(#include <print>

// Cova discovers `tests/**/*.cpp` and builds them as separate test programs.
// A test program signals failure with a non-zero exit code.
int main() {
  std::println("no tests yet");
  return 0;
}
)";
}

std::string gitignore_contents() {
  return R"(# Cova build output
/target/
/compile_commands.json

# Editor and OS noise
.DS_Store
*.swp
.idea/
.vscode/
)";
}

Status write_if_absent(const fs::Path& path, std::string_view contents, bool force) {
  if (!force && fs::exists(path)) {
    return fail(Error::make("cova::new::exists",
                            "refusing to overwrite existing file " + fs::generic_string(path))
                    .with("pass --force to overwrite"));
  }
  return fs::write_file(path, contents);
}

}  // namespace

int cmd_new(CommandContext& ctx) {
  if (ctx.positionals().empty()) {
    ctx.ui->error("`cova new` requires a project name");
    ctx.ui->info("usage: cova new <name> [--lib | --workspace]");
    return 2;
  }
  const std::string raw_name = ctx.positionals().front();
  const bool as_library = ctx.flags.boolean("lib");
  const bool as_workspace = ctx.flags.boolean("workspace");
  const bool force = ctx.flags.boolean("force");

  std::string name = sanitise_package_name(raw_name);
  if (auto explicit_name = ctx.flags.get("name"); explicit_name && !explicit_name->empty()) {
    name = *explicit_name;
  }
  std::string edition = "c++26";
  if (auto explicit_edition = ctx.flags.get("edition"); explicit_edition && !explicit_edition->empty()) {
    edition = *explicit_edition;
  }

  const auto root = ctx.cwd / raw_name;
  if (fs::exists(root) && !fs::is_directory(root)) {
    return report_error(Error::make("cova::new::exists",
                                    "a file already exists at " + fs::generic_string(root)),
                        ctx);
  }
  if (fs::is_directory(root)) {
    auto entries = fs::walk_files(root);
    bool non_empty = !entries.empty();
    if (!non_empty) {
      // Directory may contain only subdirectories; check them too.
      std::error_code ec;
      for (auto it = stdfs::directory_iterator(root, ec); it != stdfs::directory_iterator();
           it.increment(ec)) {
        non_empty = true;
        break;
      }
    }
    if (non_empty && !force) {
      return report_error(
          Error::make("cova::new::directory_not_empty",
                      "directory " + fs::generic_string(root) + " is not empty")
              .with("pass --force to create a project inside it"),
          ctx);
    }
  }

  auto dirs_status = fs::create_directories(root);
  if (!dirs_status) {
    return report_error(dirs_status.error(), ctx);
  }

  // cova.toml
  manifest::Manifest model;
  if (as_workspace) {
    model = manifest::default_workspace_manifest(name);
    auto members = fs::create_directories(root / "packages");
    if (!members) {
      return report_error(members.error(), ctx);
    }
  } else {
    model = manifest::default_manifest(name, as_library ? manifest::PackageKind::StaticLibrary
                                                        : manifest::PackageKind::Executable);
  }
  model.package.edition = edition;
  auto manifest_status = write_if_absent(root / "cova.toml", manifest::to_toml(model), force);
  if (!manifest_status) {
    return report_error(manifest_status.error(), ctx);
  }

  std::vector<fs::Path> created{root / "cova.toml"};

  if (!as_workspace) {
    // Sources
    auto src_status = fs::create_directories(root / "src");
    if (!src_status) {
      return report_error(src_status.error(), ctx);
    }
    if (as_library) {
      auto include_status = fs::create_directories(root / "include");
      if (!include_status) {
        return report_error(include_status.error(), ctx);
      }
      auto header = write_if_absent(root / "include" / (name + ".hpp"), library_header(name), force);
      if (!header) {
        return report_error(header.error(), ctx);
      }
      auto source = write_if_absent(root / "src" / (name + ".cpp"), library_source(name), force);
      if (!source) {
        return report_error(source.error(), ctx);
      }
      created.push_back(root / "include" / (name + ".hpp"));
      created.push_back(root / "src" / (name + ".cpp"));
    } else {
      auto main_file = write_if_absent(root / "src" / "main.cpp", executable_main(), force);
      if (!main_file) {
        return report_error(main_file.error(), ctx);
      }
      created.push_back(root / "src" / "main.cpp");
    }

    auto tests_status = fs::create_directories(root / "tests");
    if (!tests_status) {
      return report_error(tests_status.error(), ctx);
    }
    auto test_file = write_if_absent(root / "tests" / "smoke.cpp", test_main(), force);
    if (!test_file) {
      return report_error(test_file.error(), ctx);
    }
    created.push_back(root / "tests" / "smoke.cpp");
  }

  // Shared scaffolding.
  auto ignore_status = write_if_absent(root / ".gitignore", gitignore_contents(), force);
  if (!ignore_status) {
    return report_error(ignore_status.error(), ctx);
  }
  created.push_back(root / ".gitignore");

  auto format_status =
      write_if_absent(root / ".clang-format", formatter::default_clang_format_contents(), force);
  if (!format_status) {
    return report_error(format_status.error(), ctx);
  }
  created.push_back(root / ".clang-format");

  if (ctx.global.json) {
    json::Value out = json::Value::object();
    out.set("name", name);
    out.set("path", fs::generic_string(root));
    auto files = json::Value::array();
    for (const auto& path : created) {
      files.push(fs::generic_string(path.lexically_relative(ctx.cwd)));
    }
    out.set("files", std::move(files));
    ctx.ui->out(out.dump(2));
    return 0;
  }

  ctx.ui->info("created project `" + name + "` at " + fs::generic_string(root));
  ctx.ui->out("");
  ctx.ui->out("next steps:");
  ctx.ui->out("  cd " + raw_name);
  if (!as_workspace) {
    ctx.ui->out("  cova build");
    if (!as_library) {
      ctx.ui->out("  cova run");
    }
  }
  return 0;
}

int cmd_init(CommandContext& ctx) {
  stdfs::path root = ctx.cwd;
  if (!ctx.positionals().empty()) {
    root = fs::absolute_normalized(ctx.cwd / ctx.positionals().front());
  }
  const bool force = ctx.flags.boolean("force");

  if (!fs::is_directory(root)) {
    return report_error(
        Error::make("cova::init::missing_directory",
                    "directory " + fs::generic_string(root) + " does not exist"),
        ctx);
  }

  if (fs::is_regular_file(root / "cova.toml") && !force) {
    return report_error(
        Error::make("cova::init::already_initialised",
                    "a Cova manifest already exists at " +
                        fs::generic_string(root / "cova.toml"))
            .with("pass --force to overwrite it"),
        ctx);
  }

  std::string name = sanitise_package_name(root.filename().string());
  if (auto explicit_name = ctx.flags.get("name"); explicit_name && !explicit_name->empty()) {
    name = *explicit_name;
  }

  // Detect the existing layout (PRD §5.2 requires this rather than overwriting).
  const bool has_src = fs::is_directory(root / "src");
  const bool has_include = fs::is_directory(root / "include");
  const bool has_tests = fs::is_directory(root / "tests");
  bool has_main = false;
  if (has_src) {
    for (const auto& file : fs::walk_files(root / "src")) {
      const std::string stem = file.stem().string();
      if (stem == "main") {
        has_main = true;
        break;
      }
    }
  }

  std::vector<std::string> detected;
  if (has_src) {
    detected.emplace_back("src/");
  }
  if (has_include) {
    detected.emplace_back("include/");
  }
  if (has_tests) {
    detected.emplace_back("tests/");
  }
  if (has_main) {
    detected.emplace_back("executable entry point (src/main.cpp)");
  }

  // Existing build systems are reported, never removed.
  std::vector<std::string> foreign_builds;
  for (const auto& candidate : {"CMakeLists.txt", "meson.build", "Makefile", "BUILD.bazel"}) {
    if (fs::is_regular_file(root / candidate)) {
      foreign_builds.emplace_back(candidate);
    }
  }

  const bool as_library = ctx.flags.boolean("lib") || (has_include && !has_main);
  manifest::Manifest model =
      manifest::default_manifest(name, as_library ? manifest::PackageKind::StaticLibrary
                                                  : manifest::PackageKind::Executable);

  model.build.sources = {"src/**/*.cpp"};
  if (has_include) {
    model.build.public_includes = {"include"};
  }
  if (as_library) {
    model.package.name = name;
  }

  auto manifest_status =
      fs::write_file_atomic(root / "cova.toml", manifest::to_toml(model));
  if (!manifest_status) {
    return report_error(manifest_status.error(), ctx);
  }

  std::vector<std::string> created{"cova.toml"};
  if (!fs::exists(root / ".gitignore")) {
    auto gitignore = fs::write_file(root / ".gitignore", gitignore_contents());
    if (gitignore) {
      created.emplace_back(".gitignore");
    }
  }
  if (!fs::exists(root / ".clang-format")) {
    auto fmt_file =
        fs::write_file(root / ".clang-format", formatter::default_clang_format_contents());
    if (fmt_file) {
      created.emplace_back(".clang-format");
    }
  }

  ctx.ui->info("initialised Cova in " + fs::generic_string(root));
  for (const auto& item : detected) {
    ctx.ui->out("  detected: " + item);
  }
  if (!foreign_builds.empty()) {
    for (const auto& foreign : foreign_builds) {
      ctx.ui->warn("found existing build configuration `" + foreign +
                   "`; Cova left it untouched");
    }
    ctx.ui->info("migrating from " + foreign_builds.front() +
                 ": point `[build] sources` at the existing sources, then run `cova build`");
    ctx.ui->info("migration guide: "
                 "https://raw.githubusercontent.com/viloris-org/cova/"
                 "refs/heads/main/docs/MIGRATING.md");
  }
  if (!has_src) {
    ctx.ui->warn("no `src/` directory found; create one and add sources before building");
  }
  for (const auto& item : created) {
    ctx.ui->out("  wrote: " + item);
  }
  return 0;
}

}  // namespace cova::cli
