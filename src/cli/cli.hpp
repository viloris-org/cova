// Cova — CLI argument model and command dispatch (PRD §4).
//
// Parsing is separated from execution so every command is testable without
// spawning a process, and so global flags (§4.2) are enforced uniformly.
#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova::cli {

namespace stdfs = std::filesystem;

// Global flags from PRD §4.2. Every command must honour these identically.
struct GlobalOptions {
  std::optional<std::string> target;
  std::string profile = "dev";
  std::optional<std::string> compiler;   // family@version
  std::optional<std::string> toolchain;  // descriptor id or path
  bool offline = false;
  bool locked = false;
  bool frozen = false;
  bool no_install = false;
  bool verbose = false;
  bool quiet = false;
  int jobs = 0;
  std::string color = "auto";
  // Not in the PRD table but required for scriptability.
  bool json = false;
  bool help = false;
  std::string version_flag;  // "--version" content, unused when empty

  bool use_color(bool tty) const;
  bool install_allowed() const { return !no_install && !offline; }
  bool locked_mode() const { return locked || frozen; }
  bool offline_mode() const { return offline || frozen; }
};

enum class CommandId {
  None,
  New,
  Init,
  Build,
  Run,
  Test,
  Clean,
  Fmt,
  Check,
  Add,
  Remove,
  Fetch,
  Update,
  Tree,
  Vendor,
  Metadata,
  Graph,
  Explain,
  Install,
  Package,
  Publish,
  Doctor,
  ToolchainList,
  ToolchainInstall,
  ToolchainRemove,
  ToolchainExplain,
  ToolchainGc,
  SysrootList,
  SysrootInstall,
  SysrootVerify,
  TargetList,
  CacheGc,
  Workspace,
};

std::string_view to_string(CommandId id);
std::optional<CommandId> command_from_args(std::string_view first, std::string_view second);

// A fully parsed invocation.
struct Invocation {
  CommandId command = CommandId::None;
  GlobalOptions global;
  // Positional arguments after the subcommand (and after "--" for run/test).
  std::vector<std::string> positionals;
  // Arguments passed through to the executed program.
  std::vector<std::string> passthrough;
  // Raw flag map for command-specific options, e.g. {"--lib", "1"}.
  std::map<std::string, std::string, std::less<>> flags;
  // Original argv, for error messages.
  std::vector<std::string> argv;
};

struct ParseResult {
  Invocation invocation;
  // Non-empty when parsing failed; message is already user-facing.
  std::string error;
  // True when help/version was requested and handled.
  bool handled = false;
};

ParseResult parse(int argc, const char* const* argv);

// Renders help. `topic` is a command name or empty for the top-level help.
std::string help_text(std::string_view topic = {});
std::string version_text();

// Named flag accessors used by command implementations.
struct FlagReader {
  const std::map<std::string, std::string, std::less<>>* flags = nullptr;
  bool has(std::string_view name) const;
  std::optional<std::string> get(std::string_view name) const;
  bool boolean(std::string_view name) const;
  int integer(std::string_view name, int fallback) const;
};

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

// Output abstraction: lets tests assert on user-facing text and lets --json
// switch formats without touching command code.
class Ui {
 public:
  virtual ~Ui() = default;
  virtual void out(std::string_view text) = 0;      // normal output
  virtual void info(std::string_view text) = 0;     // status line
  virtual void warn(std::string_view text) = 0;
  virtual void error(std::string_view text) = 0;
  virtual void verbose(std::string_view text) = 0;
  virtual bool interactive() const = 0;
  // Presents a yes/no question; non-interactive defaults to `fallback`.
  virtual bool confirm(std::string_view question, bool fallback) = 0;
};

class StreamUi final : public Ui {
 public:
  StreamUi(std::ostream& out_stream, std::ostream& err_stream, bool color, bool verbose,
           bool quiet);
  void out(std::string_view text) override;
  void info(std::string_view text) override;
  void warn(std::string_view text) override;
  void error(std::string_view text) override;
  void verbose(std::string_view text) override;
  bool interactive() const override { return interactive_; }
  void set_interactive(bool value) { interactive_ = value; }
  bool confirm(std::string_view question, bool fallback) override;

 private:
  std::ostream& out_;
  std::ostream& err_;
  bool color_;
  bool verbose_;
  bool quiet_;
  bool interactive_ = false;
};

// Collected output for tests.
class StringUi final : public Ui {
 public:
  void out(std::string_view text) override;
  void info(std::string_view text) override;
  void warn(std::string_view text) override;
  void error(std::string_view text) override;
  void verbose(std::string_view text) override;
  bool interactive() const override { return is_interactive; }
  bool confirm(std::string_view question, bool fallback) override;

  std::string stdout_text;
  std::string stderr_text;
  std::vector<std::string> confirmations;
  bool is_interactive = false;
  // Answer returned when interactive; ignored otherwise.
  bool answer = false;
};

// Everything a command needs. Constructed once by the driver.
struct CommandContext {
  Invocation invocation;
  GlobalOptions global;
  FlagReader flags;
  Ui* ui = nullptr;
  // Resolved working directory and project root.
  stdfs::path cwd;
  std::optional<stdfs::path> project_root;
  // Data directory (~/.cova) after config loading.
  stdfs::path data_dir;
  // Set when running inside the Cova source tree (enables self-tests).
  bool dry_run = false;

  // Direct access to the parsed positional and passthrough arguments; commands
  // read these constantly, so they are surfaced rather than reached through
  // `invocation`.
  const std::vector<std::string>& positionals() const { return invocation.positionals; }
  const std::vector<std::string>& passthrough() const { return invocation.passthrough; }
  CommandId command() const { return invocation.command; }
};

// A command implementation. Returns the process exit code.
using CommandFn = std::function<int(CommandContext&)>;

// Registry of implementations; the driver looks up by CommandId.
const std::map<CommandId, CommandFn, std::less<>>& command_table();

// Entry point used by main(). Returns the process exit code.
int run(int argc, char** argv);
int run(int argc, char** argv, std::ostream& out_stream, std::ostream& err_stream);

// Individual commands (implemented in cmd_*.cpp).
int cmd_new(CommandContext& ctx);
int cmd_init(CommandContext& ctx);
int cmd_build(CommandContext& ctx);
int cmd_run(CommandContext& ctx);
int cmd_test(CommandContext& ctx);
int cmd_clean(CommandContext& ctx);
int cmd_fmt(CommandContext& ctx);
int cmd_check(CommandContext& ctx);
int cmd_metadata(CommandContext& ctx);
int cmd_graph(CommandContext& ctx);
int cmd_explain(CommandContext& ctx);
int cmd_doctor(CommandContext& ctx);
int cmd_toolchain(CommandContext& ctx);
int cmd_sysroot(CommandContext& ctx);
int cmd_target(CommandContext& ctx);
int cmd_cache(CommandContext& ctx);
int cmd_workspace(CommandContext& ctx);
// Dependency commands (Milestone 3 scaffolding).
int cmd_add(CommandContext& ctx);
int cmd_remove(CommandContext& ctx);
int cmd_fetch(CommandContext& ctx);
int cmd_update(CommandContext& ctx);
int cmd_tree(CommandContext& ctx);
int cmd_vendor(CommandContext& ctx);
int cmd_install(CommandContext& ctx);
int cmd_package(CommandContext& ctx);
int cmd_publish(CommandContext& ctx);

}  // namespace cova::cli
