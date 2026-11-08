// Cova — command dispatch (PRD §4).
//
// The driver owns: argv parsing, UI construction, data-directory resolution,
// command lookup, and exit-code policy. Commands own everything else.
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "cli/cli.hpp"
#include "cli/common.hpp"
#include "core/filesystem/filesystem.hpp"

namespace cova::cli {
namespace {

CommandContext make_context(Invocation invocation, Ui& ui) {
  CommandContext ctx;
  ctx.global = invocation.global;
  ctx.invocation = invocation;
  ctx.flags.flags = &ctx.invocation.flags;
  ctx.ui = &ui;

  if (auto cwd = fs::current_dir()) {
    ctx.cwd = *cwd;
  } else {
    ctx.cwd = stdfs::current_path();
  }

  if (auto dirs = fs::cova_dirs()) {
    ctx.data_dir = dirs->root;
  }
  return ctx;
}

}  // namespace

const std::map<CommandId, CommandFn, std::less<>>& command_table() {
  static const std::map<CommandId, CommandFn, std::less<>> table = {
      {CommandId::New, cmd_new},
      {CommandId::Init, cmd_init},
      {CommandId::Build, cmd_build},
      {CommandId::Run, cmd_run},
      {CommandId::Test, cmd_test},
      {CommandId::Clean, cmd_clean},
      {CommandId::Fmt, cmd_fmt},
      {CommandId::Check, cmd_check},
      {CommandId::Graph, cmd_graph},
      {CommandId::Explain, cmd_explain},
      {CommandId::Metadata, cmd_metadata},
      {CommandId::Doctor, cmd_doctor},
      {CommandId::ToolchainList, cmd_toolchain},
      {CommandId::ToolchainInstall, cmd_toolchain},
      {CommandId::ToolchainRemove, cmd_toolchain},
      {CommandId::ToolchainExplain, cmd_toolchain},
      {CommandId::ToolchainGc, cmd_toolchain},
      {CommandId::SysrootList, cmd_sysroot},
      {CommandId::SysrootInstall, cmd_sysroot},
      {CommandId::SysrootVerify, cmd_sysroot},
      {CommandId::SysrootGc, cmd_sysroot},
      {CommandId::TargetList, cmd_target},
      {CommandId::CacheGc, cmd_cache},
      {CommandId::Workspace, cmd_workspace},
      {CommandId::Add, cmd_add},
      {CommandId::Remove, cmd_remove},
      {CommandId::Fetch, cmd_fetch},
      {CommandId::Update, cmd_update},
      {CommandId::Tree, cmd_tree},
      {CommandId::Vendor, cmd_vendor},
      {CommandId::Install, cmd_install},
      {CommandId::Package, cmd_package},
      {CommandId::Publish, cmd_publish},
  };
  return table;
}

int run(int argc, char** argv, std::ostream& out_stream, std::ostream& err_stream) {
  ParseResult parsed = parse(argc, argv);

  if (!parsed.error.empty()) {
    err_stream << parsed.error << '\n';
    err_stream << "run `cova --help` for usage\n";
    return 2;
  }

  if (!parsed.invocation.global.version_flag.empty()) {
    out_stream << version_text() << '\n';
    return 0;
  }

  if (parsed.invocation.global.help || parsed.invocation.command == CommandId::None) {
    out_stream << help_text(parsed.invocation.command == CommandId::None
                                ? std::string_view{}
                                : to_string(parsed.invocation.command))
               << '\n';
    return 0;
  }

  const bool tty_out = ::isatty(STDOUT_FILENO) != 0;
  const bool color = parsed.invocation.global.use_color(tty_out);
  const bool verbose = parsed.invocation.global.verbose;
  const bool quiet = parsed.invocation.global.quiet;

  StreamUi ui(out_stream, err_stream, color, verbose, quiet);
  ui.set_interactive(::isatty(STDIN_FILENO) != 0);
  ui.set_progress_live(stderr_is_tty());

  CommandContext ctx = make_context(std::move(parsed.invocation), ui);

  const auto& table = command_table();
  auto entry = table.find(ctx.invocation.command);
  if (entry == table.end()) {
    err_stream << "error: command `" << to_string(ctx.invocation.command)
               << "` is not implemented in this build\n";
    return 2;
  }

  try {
    return entry->second(ctx);
  } catch (const std::exception& error) {
    // Commands return errors rather than throwing; reaching here means a bug.
    err_stream << "error: internal error: " << error.what() << '\n';
    err_stream << "this is a bug in Cova; please report it\n";
    return 70;
  }
}

int run(int argc, char** argv) { return run(argc, argv, std::cout, std::cerr); }

}  // namespace cova::cli
