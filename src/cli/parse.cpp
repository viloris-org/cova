// Cova — CLI argument parsing and help rendering (PRD §4).
//
// Parsing is deliberately a single pass: every argument is classified once into
// a global option, a command option, a positional, or a `--` passthrough token.
// This keeps "flags before the subcommand" and "flags after the subcommand"
// behaving identically, which PRD §4.2 requires for global options.
#include "cli/cli.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace cova::cli {
namespace {

constexpr std::string_view kVersionString = "0.1.0";

struct OptionSpec {
  std::string_view name;
  bool takes_value;
  bool global;  // accepted before the subcommand and anywhere after it
};

// Every option the CLI understands. Order is irrelevant; lookup is linear over a
// small constant table, which is faster than a map at this size.
constexpr std::array kOptionSpecs = {
    // Global (§4.2)
    OptionSpec{"target", true, true},
    OptionSpec{"profile", true, true},
    OptionSpec{"compiler", true, true},
    OptionSpec{"toolchain", true, true},
    OptionSpec{"offline", false, true},
    OptionSpec{"locked", false, true},
    OptionSpec{"frozen", false, true},
    OptionSpec{"no-install", false, true},
    OptionSpec{"verbose", false, true},
    OptionSpec{"quiet", false, true},
    OptionSpec{"jobs", true, true},
    OptionSpec{"color", true, true},
    OptionSpec{"json", false, true},
    OptionSpec{"help", false, true},
    OptionSpec{"version", false, true},

    // new / init
    OptionSpec{"lib", false, false},
    OptionSpec{"workspace", false, false},
    OptionSpec{"name", true, false},
    OptionSpec{"edition", true, false},
    OptionSpec{"force", false, false},
    OptionSpec{"template", true, false},

    // build / run / test / check
    OptionSpec{"release", false, false},
    OptionSpec{"debug", false, false},
    OptionSpec{"package", true, false},
    OptionSpec{"bin", true, false},
    OptionSpec{"example", true, false},
    OptionSpec{"dry-run", false, false},
    OptionSpec{"keep-going", false, false},
    OptionSpec{"runner", true, false},
    OptionSpec{"filter", true, false},
    OptionSpec{"manifest-path", true, false},
    OptionSpec{"target-dir", true, false},
    OptionSpec{"sysroot", true, false},
    OptionSpec{"from", true, false},

    // clean
    OptionSpec{"all", false, false},

    // fmt
    OptionSpec{"check", false, false},
    OptionSpec{"diff", false, false},
    OptionSpec{"style", true, false},
    OptionSpec{"tool", true, false},

    // graph / explain / metadata
    OptionSpec{"format", true, false},
    OptionSpec{"explain", false, false},

    // dependencies (M3 surface)
    OptionSpec{"dev", false, false},
    OptionSpec{"path", true, false},
    OptionSpec{"git", true, false},
    OptionSpec{"rev", true, false},
    OptionSpec{"features", true, false},
    OptionSpec{"registry", true, false},
    OptionSpec{"optional", false, false},

    // toolchain / sysroot
    OptionSpec{"list", false, false},
    OptionSpec{"verify", false, false},
    OptionSpec{"gc", false, false},
    OptionSpec{"yes", false, false},
    OptionSpec{"install", false, false},

    // package / publish / install commands
    OptionSpec{"output", true, false},
    OptionSpec{"prune", false, false},
};

const OptionSpec* find_option(std::string_view name) {
  for (const auto& spec : kOptionSpecs) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

// Short aliases.
struct ShortAlias {
  char letter;
  std::string_view long_name;
};
constexpr std::array kShortAliases = {
    ShortAlias{'v', "verbose"},   ShortAlias{'q', "quiet"},  ShortAlias{'h', "help"},
    ShortAlias{'V', "version"},   ShortAlias{'j', "jobs"},   ShortAlias{'p', "package"},
    ShortAlias{'C', "color"},     ShortAlias{'F', "features"},
};

std::string_view long_name_for_short(char letter) {
  for (const auto& alias : kShortAliases) {
    if (alias.letter == letter) {
      return alias.long_name;
    }
  }
  return {};
}

struct CommandName {
  std::string_view first;
  std::string_view second;
  CommandId id;
};

constexpr std::array kCommandNames = {
    CommandName{"new", "", CommandId::New},
    CommandName{"init", "", CommandId::Init},
    CommandName{"build", "", CommandId::Build},
    CommandName{"run", "", CommandId::Run},
    CommandName{"test", "", CommandId::Test},
    CommandName{"clean", "", CommandId::Clean},
    CommandName{"fmt", "", CommandId::Fmt},
    CommandName{"format", "", CommandId::Fmt},
    CommandName{"check", "", CommandId::Check},
    CommandName{"add", "", CommandId::Add},
    CommandName{"remove", "", CommandId::Remove},
    CommandName{"rm", "", CommandId::Remove},
    CommandName{"fetch", "", CommandId::Fetch},
    CommandName{"update", "", CommandId::Update},
    CommandName{"tree", "", CommandId::Tree},
    CommandName{"vendor", "", CommandId::Vendor},
    CommandName{"metadata", "", CommandId::Metadata},
    CommandName{"graph", "", CommandId::Graph},
    CommandName{"explain", "", CommandId::Explain},
    CommandName{"install", "", CommandId::Install},
    CommandName{"package", "", CommandId::Package},
    CommandName{"publish", "", CommandId::Publish},
    CommandName{"doctor", "", CommandId::Doctor},
    CommandName{"workspace", "", CommandId::Workspace},
    CommandName{"toolchain", "list", CommandId::ToolchainList},
    CommandName{"toolchain", "install", CommandId::ToolchainInstall},
    CommandName{"toolchain", "remove", CommandId::ToolchainRemove},
    CommandName{"toolchain", "explain", CommandId::ToolchainExplain},
    CommandName{"toolchain", "gc", CommandId::ToolchainGc},
    CommandName{"sysroot", "list", CommandId::SysrootList},
    CommandName{"sysroot", "install", CommandId::SysrootInstall},
    CommandName{"sysroot", "verify", CommandId::SysrootVerify},
    CommandName{"sysroot", "gc", CommandId::SysrootVerify},
    CommandName{"target", "list", CommandId::TargetList},
    CommandName{"cache", "gc", CommandId::CacheGc},
};

constexpr std::array kCommandGroups = {"toolchain", "sysroot", "target", "cache"};

bool is_group(std::string_view name) {
  for (auto group : kCommandGroups) {
    if (group == name) {
      return true;
    }
  }
  return false;
}

// Commands that accept `-- args...` for the executed program.
bool accepts_passthrough(CommandId id) {
  return id == CommandId::Run || id == CommandId::Test;
}

}  // namespace

std::string_view to_string(CommandId id) {
  switch (id) {
    case CommandId::None:
      return "none";
    case CommandId::New:
      return "new";
    case CommandId::Init:
      return "init";
    case CommandId::Build:
      return "build";
    case CommandId::Run:
      return "run";
    case CommandId::Test:
      return "test";
    case CommandId::Clean:
      return "clean";
    case CommandId::Fmt:
      return "fmt";
    case CommandId::Check:
      return "check";
    case CommandId::Add:
      return "add";
    case CommandId::Remove:
      return "remove";
    case CommandId::Fetch:
      return "fetch";
    case CommandId::Update:
      return "update";
    case CommandId::Tree:
      return "tree";
    case CommandId::Vendor:
      return "vendor";
    case CommandId::Metadata:
      return "metadata";
    case CommandId::Graph:
      return "graph";
    case CommandId::Explain:
      return "explain";
    case CommandId::Install:
      return "install";
    case CommandId::Package:
      return "package";
    case CommandId::Publish:
      return "publish";
    case CommandId::Doctor:
      return "doctor";
    case CommandId::Workspace:
      return "workspace";
    case CommandId::ToolchainList:
      return "toolchain list";
    case CommandId::ToolchainInstall:
      return "toolchain install";
    case CommandId::ToolchainRemove:
      return "toolchain remove";
    case CommandId::ToolchainExplain:
      return "toolchain explain";
    case CommandId::ToolchainGc:
      return "toolchain gc";
    case CommandId::SysrootList:
      return "sysroot list";
    case CommandId::SysrootInstall:
      return "sysroot install";
    case CommandId::SysrootVerify:
      return "sysroot verify";
    case CommandId::TargetList:
      return "target list";
    case CommandId::CacheGc:
      return "cache gc";
  }
  return "none";
}

std::optional<CommandId> command_from_args(std::string_view first, std::string_view second) {
  for (const auto& entry : kCommandNames) {
    if (entry.first != first) {
      continue;
    }
    if (entry.second.empty()) {
      if (!second.empty()) {
        continue;  // has no subcommands
      }
      return entry.id;
    }
    if (entry.second == second) {
      return entry.id;
    }
  }
  return std::nullopt;
}

bool GlobalOptions::use_color(bool tty) const {
  if (color == "always") {
    return true;
  }
  if (color == "never") {
    return false;
  }
  return tty;
}

// ---------------------------------------------------------------------------
// FlagReader
// ---------------------------------------------------------------------------
bool FlagReader::has(std::string_view name) const {
  return flags != nullptr && flags->find(name) != flags->end();
}

std::optional<std::string> FlagReader::get(std::string_view name) const {
  if (flags == nullptr) {
    return std::nullopt;
  }
  auto it = flags->find(name);
  if (it == flags->end()) {
    return std::nullopt;
  }
  return it->second;
}

bool FlagReader::boolean(std::string_view name) const {
  auto value = get(name);
  if (!value) {
    return false;
  }
  return !(*value == "0" || *value == "false" || *value == "no");
}

int FlagReader::integer(std::string_view name, int fallback) const {
  auto value = get(name);
  if (!value) {
    return fallback;
  }
  try {
    std::size_t consumed = 0;
    int parsed = std::stoi(*value, &consumed);
    if (consumed != value->size()) {
      return fallback;
    }
    return parsed;
  } catch (...) {
    return fallback;
  }
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
namespace {

struct Token {
  enum class Kind { Flag, Value };
  Kind kind;
  std::string name;      // for flags
  std::string value;     // inline value for --flag=value, or the raw token
  bool has_inline_value = false;
};

// Applies one global option to the option struct. Returns an error message when
// the value is invalid.
std::string apply_global(GlobalOptions& g, std::string_view name, const std::string& value,
                         bool has_value) {
  if (name == "target") {
    g.target = value;
  } else if (name == "profile") {
    g.profile = value.empty() ? "dev" : value;
  } else if (name == "compiler") {
    g.compiler = value;
  } else if (name == "toolchain") {
    g.toolchain = value;
  } else if (name == "offline") {
    g.offline = true;
  } else if (name == "locked") {
    g.locked = true;
  } else if (name == "frozen") {
    g.frozen = true;
  } else if (name == "no-install") {
    g.no_install = true;
  } else if (name == "verbose") {
    g.verbose = true;
  } else if (name == "quiet") {
    g.quiet = true;
  } else if (name == "json") {
    g.json = true;
  } else if (name == "jobs") {
    if (!has_value || value.empty()) {
      return "error: `--jobs` requires a number";
    }
    try {
      std::size_t consumed = 0;
      int parsed = std::stoi(value, &consumed);
      if (consumed != value.size() || parsed < 1) {
        return "error: `--jobs` expects a positive integer";
      }
      g.jobs = parsed;
    } catch (...) {
      return "error: `--jobs` expects a positive integer";
    }
  } else if (name == "color") {
    g.color = value;
    if (g.color != "auto" && g.color != "always" && g.color != "never") {
      return "error: `--color` expects auto, always or never";
    }
  }
  return {};
}

}  // namespace

ParseResult parse(int argc, const char* const* argv) {
  ParseResult result;
  Invocation& inv = result.invocation;
  for (int i = 0; i < argc; ++i) {
    inv.argv.emplace_back(argv[i] == nullptr ? "" : argv[i]);
  }

  // Collect tokens.
  std::vector<Token> tokens;
  bool after_double_dash = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i] == nullptr ? "" : argv[i];
    if (after_double_dash) {
      inv.passthrough.emplace_back(arg);
      continue;
    }
    if (arg == "--") {
      after_double_dash = true;
      continue;
    }
    if (arg.size() >= 2 && arg[0] == '-') {
      Token token{Token::Kind::Flag, {}, std::string(arg), false};
      if (arg.size() >= 3 && arg[1] == '-') {
        std::string_view body = arg.substr(2);
        auto eq = body.find('=');
        if (eq != std::string_view::npos) {
          token.name = std::string(body.substr(0, eq));
          token.value = std::string(body.substr(eq + 1));
          token.has_inline_value = true;
        } else {
          token.name = std::string(body);
        }
      } else {
        // Short flag: -j8, -pname, or -v.
        std::string_view body = arg.substr(1);
        std::string_view long_name = long_name_for_short(body.front());
        if (long_name.empty()) {
          result.error = "error: unrecognised option `" + std::string(arg) + "`";
          return result;
        }
        token.name = std::string(long_name);
        if (body.size() > 1) {
          token.value = std::string(body.substr(1));
          token.has_inline_value = true;
        }
      }
      tokens.push_back(std::move(token));
      continue;
    }
    Token token{Token::Kind::Value, {}, std::string(arg), false};
    tokens.push_back(std::move(token));
  }

  // Walk tokens, attributing flags to global options or command options. The
  // command is decided by the first positional, so global flags may appear on
  // either side of it.
  std::vector<std::string> positionals;
  std::vector<std::pair<std::string, std::optional<std::string>>> pending;  // name, value
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    Token& token = tokens[i];
    if (token.kind == Token::Kind::Value) {
      positionals.push_back(std::move(token.value));
      continue;
    }
    const OptionSpec* spec = find_option(token.name);
    if (spec == nullptr) {
      result.error = "error: unrecognised option `--" + token.name + "`";
      return result;
    }
    if (spec->name == "help") {
      inv.global.help = true;
      continue;
    }
    if (spec->name == "version") {
      inv.global.version_flag = std::string(kVersionString);
      continue;
    }
    std::optional<std::string> value;
    if (token.has_inline_value) {
      value = std::move(token.value);
    } else if (spec->takes_value) {
      if (i + 1 >= tokens.size() || tokens[i + 1].kind != Token::Kind::Value) {
        result.error = "error: option `--" + token.name + "` requires a value";
        return result;
      }
      value = std::move(tokens[++i].value);
    }
    pending.emplace_back(token.name, std::move(value));
  }

  if (!result.error.empty()) {
    return result;
  }
  if (inv.global.help) {
    result.handled = true;
    return result;
  }
  if (!inv.global.version_flag.empty()) {
    result.handled = true;
    return result;
  }

  // Resolve the command from the leading positionals.
  if (positionals.empty()) {
    result.error = "error: a command is required (run `cova --help`)";
    return result;
  }
  std::string_view first = positionals[0];
  std::string_view second;
  std::size_t consumed = 0;
  if (is_group(first)) {
    if (positionals.size() < 2) {
      // `cova toolchain` alone means "list" for the friendly cases.
      if (first == "toolchain" || first == "sysroot" || first == "target") {
        second = "list";
        consumed = 1;
        auto id = command_from_args(first, second);
        if (!id) {
          result.error = "error: `" + std::string(first) + "` requires a subcommand";
          return result;
        }
        inv.command = *id;
      } else {
        result.error = "error: `" + std::string(first) + "` requires a subcommand";
        return result;
      }
    } else {
      second = positionals[1];
      auto id = command_from_args(first, second);
      if (!id) {
        result.error = "error: unrecognised subcommand `" + std::string(first) + " " +
                       std::string(second) + "`";
        return result;
      }
      inv.command = *id;
      consumed = 2;
    }
  } else {
    auto id = command_from_args(first, {});
    if (!id) {
      result.error = "error: unrecognised command `" + std::string(first) + "`";
      return result;
    }
    inv.command = *id;
    consumed = 1;
  }
  inv.positionals.assign(positionals.begin() + static_cast<std::ptrdiff_t>(consumed),
                         positionals.end());

  // Route flags. Global ones always land in GlobalOptions, even when they
  // appeared before the command was known.
  for (auto& [name, value] : pending) {
    const OptionSpec* spec = find_option(name);
    if (spec != nullptr && spec->global) {
      std::string error = apply_global(inv.global, name, value.value_or(""), value.has_value());
      if (!error.empty()) {
        result.error = std::move(error);
        return result;
      }
      continue;
    }
    inv.flags[name] = value.value_or("1");
  }

  if (!accepts_passthrough(inv.command) && !inv.passthrough.empty()) {
    result.error = "error: `cova " + std::string(to_string(inv.command)) +
                   "` does not accept arguments after `--`";
    return result;
  }
  return result;
}

std::string version_text() {
  return "cova " + std::string(kVersionString);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
namespace {

std::string top_level_help() {
  return "cova " + std::string(kVersionString) +
         " — Cross-Compilation-First, Cargo-Inspired C/C++ Toolchain\n"
         "\n"
         "Usage: cova <command> [options]\n"
         "\n"
         "Project:\n"
         "  new <name>             Create a new project\n"
         "  init                   Initialise an existing directory\n"
         "  metadata               Print project metadata\n"
         "  workspace              Workspace management\n"
         "\n"
         "Build and development:\n"
         "  build                  Build the project\n"
         "  run                    Build and run\n"
         "  test                   Build and run tests\n"
         "  check                  Compile without linking\n"
         "  clean                  Remove build artifacts\n"
         "  fmt                    Format code with clang-format\n"
         "  graph                  Show the build graph\n"
         "  explain                Explain build and cache decisions\n"
         "\n"
         "Dependencies:\n"
         "  add <name>             Add a dependency\n"
         "  remove <name>          Remove a dependency\n"
         "  fetch                  Fetch dependency sources\n"
         "  update                 Re-resolve dependencies\n"
         "  tree                   Show the dependency tree\n"
         "  vendor                 Export dependency sources into the project\n"
         "\n"
         "Toolchains and targets:\n"
         "  toolchain list         List discovered toolchains\n"
         "  toolchain install      Install a managed toolchain\n"
         "  toolchain remove       Remove a managed toolchain\n"
         "  toolchain explain      Explain the toolchain selection\n"
         "  toolchain gc           Reclaim unused managed components\n"
         "  sysroot list           List installed sysroots\n"
         "  sysroot install        Install a sysroot\n"
         "  sysroot verify         Verify an installed sysroot\n"
         "  target list            List supported targets\n"
         "  doctor                 Diagnose the development environment\n"
         "\n"
         "Release:\n"
         "  install, package, publish, cache gc\n"
         "\n"
         "Global options:\n"
         "  --target <triple>      Build for a target platform\n"
         "  --profile <name>       Build profile (dev, release)\n"
         "  --compiler <family@ver>  Compiler family with optional version\n"
         "  --toolchain <id>       Use a specific toolchain identity\n"
         "  --offline              Forbid network access\n"
         "  --locked               Refuse to change the lockfile\n"
         "  --frozen               Equivalent to --offline --locked\n"
         "  --no-install           Never install new components\n"
         "  -j, --jobs <n>         Parallel jobs\n"
         "  -v, --verbose          Verbose output\n"
         "  -q, --quiet            Suppress status output\n"
         "  --color <when>         auto, always, never\n"
         "  --json                 Machine-readable output where supported\n"
         "  -h, --help             Print help\n"
         "  -V, --version          Print version\n"
         "\n"
         "Run `cova <command> --help` for command-specific options.\n";
}

std::string usage_block(std::string_view cmd, std::string_view synopsis,
                        std::string_view body) {
  return "Usage: cova " + std::string(cmd) + " " + std::string(synopsis) + "\n\n" +
         std::string(body);
}

}  // namespace

std::string help_text(std::string_view topic) {
  if (topic.empty()) {
    return top_level_help();
  }
  if (topic == "new") {
    return usage_block("new", "[options] <name>",
                       "Create a new Cova project (PRD §5.1).\n\n"
                       "Options:\n"
                       "  --lib                Create a static library instead of an executable\n"
                       "  --workspace          Create a workspace root\n"
                       "  --name <name>        Override the package name\n"
                       "  --edition <edition>  C++ standard (default: c++26)\n"
                       "  --force              Allow writing into a non-empty directory\n");
  }
  if (topic == "init") {
    return usage_block("init", "[options] [path]",
                       "Initialise Cova in an existing project without overwriting files (§5.2).\n\n"
                       "Options:\n"
                       "  --lib                Assume a library layout\n"
                       "  --force              Overwrite an existing cova.toml\n"
                       "  --name <name>        Override the derived package name\n");
  }
  if (topic == "build") {
    return usage_block("build", "[options]",
                       "Build the current package, or the workspace with --workspace.\n\n"
                       "Options:\n"
                       "  --release            Use the release profile\n"
                       "  --workspace          Build every workspace member\n"
                       "  -p, --package <name> Build a specific member\n"
                       "  --target <triple>    Cross-compile for a target\n"
                       "  --dry-run            Plan without executing\n"
                       "  --keep-going         Continue after a failed action\n"
                       "  --json               Emit machine-readable output\n");
  }
  if (topic == "check") {
    return usage_block("check", "[options]",
                       "Compile every translation unit without linking (fast feedback).\n\n"
                       "Options:\n"
                       "  --release            Use the release profile\n"
                       "  --workspace          Check every workspace member\n");
  }
  if (topic == "run") {
    return usage_block("run", "[options] [-- args...]",
                       "Build then run the primary executable.\n\n"
                       "Options:\n"
                       "  --release            Use the release profile\n"
                       "  --bin <name>         Select an executable\n"
                       "  --runner <name>      native, qemu, wine or remote (§13.3)\n"
                       "  --                   Pass the remaining arguments to the program\n");
  }
  if (topic == "test") {
    return usage_block("test", "[options] [-- args...]",
                       "Build and execute declared test programs (§13.2).\n\n"
                       "Options:\n"
                       "  --workspace          Test every member\n"
                       "  -p, --package <name> Test one member\n"
                       "  --runner <name>      native, qemu, wine or remote\n"
                       "  --filter <pattern>   Test name filter passed to the runner\n");
  }
  if (topic == "clean") {
    return usage_block("clean", "[options]",
                       "Remove build artifacts. Never removes shared caches (§12.1).\n\n"
                       "Options:\n"
                       "  --release            Clean only the release profile\n"
                       "  --all                Remove the whole target directory\n"
                       "  --dry-run            Show what would be removed\n");
  }
  if (topic == "fmt") {
    return usage_block("fmt", "[options]",
                       "Format sources with clang-format (§11).\n\n"
                       "Options:\n"
                       "  --check              Do not write; exit non-zero when unformatted\n"
                       "  --diff               Print a diff instead of writing\n"
                       "  --all                Format the whole workspace\n"
                       "  --workspace          Format every workspace member\n"
                       "  --style <style>      clang-format style or `file`\n"
                       "  --json               Machine-readable results\n");
  }
  if (topic == "doctor") {
    return usage_block("doctor", "[options]",
                       "Diagnose the environment: toolchains, sysroots, formatter, paths.\n");
  }
  if (topic == "toolchain") {
    return usage_block("toolchain", "<list|install|remove|explain|gc> [options]",
                       "Manage toolchains (§7).\n\n"
                       "  cova toolchain list [--json]        List discovered toolchains\n"
                       "  cova toolchain explain --target T  Explain the selection chain\n"
                       "  cova toolchain install <id>         Install a managed component\n"
                       "  cova toolchain remove <id>          Remove a managed component\n"
                       "  cova toolchain gc [--dry-run]       Reclaim unused components\n");
  }
  if (topic == "sysroot") {
    return usage_block("sysroot", "<list|install|verify> [options]",
                       "Manage sysroots (§8).\n\n"
                       "  cova sysroot list [--json]              List installed sysroots\n"
                       "  cova sysroot install --target T [--from <archive>]\n"
                       "  cova sysroot verify [<id>]              Verify integrity\n");
  }
  if (topic == "target") {
    return usage_block("target", "list [--json]",
                       "List the targets Cova supports first-class (§20).\n");
  }
  if (topic == "graph") {
    return usage_block("graph", "[options]",
                       "Print the build graph (§4.1).\n\n"
                       "Options:\n"
                       "  --format <text|dot|json>   Output format\n");
  }
  if (topic == "explain") {
    return usage_block("explain", "[options]",
                       "Explain toolchain selection and build decisions (§19.3).\n\n"
                       "Options:\n"
                       "  --json                 Emit machine-readable output\n");
  }
  if (topic == "metadata") {
    return usage_block("metadata", "[options]",
                       "Print project metadata (§4.1).\n\n"
                       "Options:\n"
                       "  --json                 JSON output\n");
  }
  return top_level_help();
}

}  // namespace cova::cli
