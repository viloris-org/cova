// Cova — CLI parsing and UI behaviour (PRD §4, §19.3).
//
// Parsing is tested directly rather than through the driver: these tests pin the
// argument grammar that every script and user depends on.
#include <string>
#include <vector>

#include "cli/cli.hpp"
#include "testing/test.hpp"

namespace {

using cova::cli::CommandId;
using cova::cli::parse;

std::vector<const char*> argv_of(const std::vector<std::string>& args) {
  std::vector<const char*> out;
  out.reserve(args.size());
  for (const auto& arg : args) {
    out.push_back(arg.c_str());
  }
  return out;
}

cova::cli::ParseResult parse_args(std::vector<std::string> args) {
  auto pointers = argv_of(args);
  return parse(static_cast<int>(pointers.size()), pointers.data());
}

}  // namespace

COVA_TEST_NAMED(cli, parses_bare_command) {
  auto result = parse_args({"cova", "build"});
  COVA_CHECK(result.error.empty());
  COVA_CHECK(result.invocation.command == CommandId::Build);
  COVA_CHECK(result.invocation.positionals.empty());
}

COVA_TEST_NAMED(cli, parses_new_with_name) {
  auto result = parse_args({"cova", "new", "hello"});
  COVA_CHECK(result.error.empty());
  COVA_CHECK(result.invocation.command == CommandId::New);
  COVA_CHECK_EQ(result.invocation.positionals.size(), std::size_t{1});
  COVA_CHECK_EQ(result.invocation.positionals.front(), std::string("hello"));
}

COVA_TEST_NAMED(cli, parses_command_groups) {
  auto list = parse_args({"cova", "toolchain", "list"});
  COVA_CHECK(list.error.empty());
  COVA_CHECK(list.invocation.command == CommandId::ToolchainList);

  auto explain = parse_args({"cova", "toolchain", "explain"});
  COVA_CHECK(explain.invocation.command == CommandId::ToolchainExplain);

  auto sysroot = parse_args({"cova", "sysroot", "verify"});
  COVA_CHECK(sysroot.invocation.command == CommandId::SysrootVerify);

  // `gc` is a distinct command; it must not silently run `verify`.
  auto sysroot_gc = parse_args({"cova", "sysroot", "gc"});
  COVA_CHECK(sysroot_gc.error.empty());
  COVA_CHECK(sysroot_gc.invocation.command == CommandId::SysrootGc);
  COVA_CHECK(sysroot_gc.invocation.command != CommandId::SysrootVerify);

  auto targets = parse_args({"cova", "target", "list"});
  COVA_CHECK(targets.invocation.command == CommandId::TargetList);
}

COVA_TEST_NAMED(cli, bare_group_defaults_to_list) {
  auto result = parse_args({"cova", "toolchain"});
  COVA_CHECK(result.error.empty());
  COVA_CHECK(result.invocation.command == CommandId::ToolchainList);
}

COVA_TEST_NAMED(cli, global_flags_before_and_after_command) {
  auto before = parse_args({"cova", "--target", "aarch64-unknown-linux-musl", "build"});
  COVA_CHECK(before.error.empty());
  COVA_CHECK(before.invocation.command == CommandId::Build);
  COVA_CHECK(before.invocation.global.target.has_value());
  COVA_CHECK_EQ(*before.invocation.global.target,
                std::string("aarch64-unknown-linux-musl"));

  auto after = parse_args({"cova", "build", "--target=aarch64-unknown-linux-musl"});
  COVA_CHECK(after.error.empty());
  COVA_CHECK_EQ(*after.invocation.global.target,
                std::string("aarch64-unknown-linux-musl"));

  // Both sides may be used together; the later value wins.
  auto both = parse_args({"cova", "--target", "x86_64-unknown-linux-gnu", "build", "--target",
                          "aarch64-unknown-linux-musl"});
  COVA_CHECK_EQ(*both.invocation.global.target,
                std::string("aarch64-unknown-linux-musl"));
}

COVA_TEST_NAMED(cli, parses_offline_locked_frozen) {
  auto offline = parse_args({"cova", "build", "--offline"});
  COVA_CHECK(offline.invocation.global.offline);
  COVA_CHECK(!offline.invocation.global.install_allowed());

  auto frozen = parse_args({"cova", "build", "--frozen"});
  COVA_CHECK(frozen.invocation.global.frozen);
  COVA_CHECK(frozen.invocation.global.offline_mode());
  COVA_CHECK(frozen.invocation.global.locked_mode());

  auto no_install = parse_args({"cova", "build", "--no-install"});
  COVA_CHECK(!no_install.invocation.global.install_allowed());
}

COVA_TEST_NAMED(cli, parses_jobs_short_and_long) {
  auto long_form = parse_args({"cova", "build", "--jobs", "8"});
  COVA_CHECK_EQ(long_form.invocation.global.jobs, 8);

  auto joined_short = parse_args({"cova", "build", "-j8"});
  COVA_CHECK_EQ(joined_short.invocation.global.jobs, 8);

  auto separate_short = parse_args({"cova", "build", "-j", "4"});
  COVA_CHECK_EQ(separate_short.invocation.global.jobs, 4);

  auto invalid = parse_args({"cova", "build", "--jobs", "lots"});
  COVA_CHECK(!invalid.error.empty());
}

COVA_TEST_NAMED(cli, rejects_unknown_command) {
  auto result = parse_args({"cova", "frobnicate"});
  COVA_CHECK(!result.error.empty());
  COVA_CHECK(result.error.find("frobnicate") != std::string::npos);
}

COVA_TEST_NAMED(cli, rejects_unknown_option) {
  auto result = parse_args({"cova", "build", "--nonsense"});
  COVA_CHECK(!result.error.empty());
  COVA_CHECK(result.error.find("nonsense") != std::string::npos);
}

COVA_TEST_NAMED(cli, requires_value_for_value_option) {
  auto result = parse_args({"cova", "build", "--target"});
  COVA_CHECK(!result.error.empty());
  COVA_CHECK(result.error.find("requires a value") != std::string::npos);
}

COVA_TEST_NAMED(cli, passthrough_only_for_run_and_test) {
  auto run = parse_args({"cova", "run", "--", "--flag", "value"});
  COVA_CHECK(run.error.empty());
  COVA_CHECK_EQ(run.invocation.passthrough.size(), std::size_t{2});
  COVA_CHECK_EQ(run.invocation.passthrough[0], std::string("--flag"));

  auto build = parse_args({"cova", "build", "--", "junk"});
  COVA_CHECK(!build.error.empty());
}

COVA_TEST_NAMED(cli, command_flags_recorded) {
  auto result = parse_args({"cova", "new", "app", "--lib", "--edition", "c++23"});
  COVA_CHECK(result.error.empty());
  cova::cli::FlagReader reader;
  reader.flags = &result.invocation.flags;
  COVA_CHECK(reader.boolean("lib"));
  COVA_CHECK_EQ(*reader.get("edition"), std::string("c++23"));
  COVA_CHECK(!reader.boolean("workspace"));
}

COVA_TEST_NAMED(cli, help_and_version_are_handled) {
  auto help = parse_args({"cova", "--help"});
  COVA_CHECK(help.handled);
  COVA_CHECK(help.invocation.global.help);

  auto version = parse_args({"cova", "--version"});
  COVA_CHECK(version.handled);
  COVA_CHECK(!version.invocation.global.version_flag.empty());

  auto sub_help = parse_args({"cova", "build", "--help"});
  COVA_CHECK(sub_help.handled);
  COVA_CHECK((sub_help.invocation.command) == CommandId::Build);
}

COVA_TEST_NAMED(cli, no_arguments_requests_help) {
  auto result = parse_args({"cova"});
  COVA_CHECK(result.handled);
  COVA_CHECK(result.invocation.global.help);
}

COVA_TEST_NAMED(cli, colour_policy) {
  auto result = parse_args({"cova", "build", "--color", "never"});
  COVA_CHECK(!result.invocation.global.use_color(true));
  COVA_CHECK(!result.invocation.global.use_color(false));

  auto always = parse_args({"cova", "build", "--color", "always"});
  COVA_CHECK(always.invocation.global.use_color(false));

  auto automatic = parse_args({"cova", "build"});
  COVA_CHECK(automatic.invocation.global.use_color(true));
  COVA_CHECK(!automatic.invocation.global.use_color(false));

  auto invalid = parse_args({"cova", "build", "--color", "rainbow"});
  COVA_CHECK(!invalid.error.empty());
}

COVA_TEST_NAMED(cli, help_text_mentions_commands) {
  const std::string top = cova::cli::help_text();
  COVA_CHECK(top.find("cova new") != std::string::npos ||
             top.find("new <name>") != std::string::npos);
  COVA_CHECK(top.find("--offline") != std::string::npos);
  COVA_CHECK(top.find("--target") != std::string::npos);

  const std::string build = cova::cli::help_text("build");
  COVA_CHECK(build.find("--release") != std::string::npos);
}

COVA_TEST_NAMED(cli, string_ui_collects_streams) {
  cova::cli::StringUi ui;
  ui.out("plain");
  ui.info("status");
  ui.warn("careful");
  ui.error("bad");

  COVA_CHECK(ui.stdout_text.find("plain") != std::string::npos);
  COVA_CHECK(ui.stdout_text.find("status") != std::string::npos);
  COVA_CHECK(ui.stderr_text.find("warning: careful") != std::string::npos);
  COVA_CHECK(ui.stderr_text.find("error: bad") != std::string::npos);
  // Non-interactive confirmation falls back without prompting.
  COVA_CHECK(!ui.confirm("continue?", false));
  COVA_CHECK(ui.confirm("continue?", true));
}

COVA_TEST_NAMED(cli, version_text_is_semver) {
  const std::string version = cova::cli::version_text();
  COVA_CHECK(version.rfind("cova ", 0) == 0);
  COVA_CHECK(version.find('.') != std::string::npos);
}

COVA_TEST_MAIN()
