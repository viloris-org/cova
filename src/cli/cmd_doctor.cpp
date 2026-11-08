// Cova — `cova doctor`, `cova target list`, `cova toolchain …`, `cova sysroot …`
// (PRD §4.1, §7, §8, §19.3).
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "cli/common.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/json/json.hpp"
#include "core/process/process.hpp"
#include "formatter/formatter.hpp"
#include "sysroot/manager/manager.hpp"
#include "toolchain/discovery/discovery.hpp"
#include "toolchain/installer/installer.hpp"
#include "toolchain/probe/probe.hpp"
#include "toolchain/resolver/resolver.hpp"

namespace cova::cli {
namespace {

json::Value triple_to_json(const platform::Triple& triple) {
  json::Value out = json::Value::object();
  out.set("triple", triple.to_string());
  out.set("arch", std::string(platform::to_string(triple.arch)));
  out.set("vendor", std::string(platform::to_string(triple.vendor)));
  out.set("os", std::string(platform::to_string(triple.os)));
  out.set("env", std::string(platform::to_string(triple.env)));
  out.set("abi", std::string(platform::to_string(triple.abi())));
  out.set("crt", std::string(platform::to_string(triple.crt())));
  return out;
}

std::string describe_toolchain_row(const toolchain::ToolchainDescriptor& descriptor) {
  std::string row = descriptor.label();
  if (!descriptor.component_path("cxx").empty()) {
    row += "  ";
    row += fs::generic_string(descriptor.component_path("cxx"));
  }
  return row;
}

}  // namespace

int cmd_target(CommandContext& ctx) {
  const auto& targets = platform::known_targets();
  const auto& host = platform::host_triple();

  if (ctx.global.json) {
    json::Value root = json::Value::object();
    root.set("host", triple_to_json(host));
    auto array = json::Value::array();
    for (const auto& target : targets) {
      array.push(triple_to_json(target));
    }
    root.set("targets", std::move(array));
    ctx.ui->out(root.dump(2));
    return 0;
  }

  ctx.ui->out("host: " + host.to_string());
  ctx.ui->out("");
  ctx.ui->out("supported targets:");
  std::size_t width = 0;
  for (const auto& target : targets) {
    width = std::max(width, target.to_string().size());
  }
  for (const auto& target : targets) {
    std::string line = "  " + target.to_string();
    line.append(width - target.to_string().size(), ' ');
    line += "  ";
    line += target.to_display_string();
    if (target == host) {
      line += "  (native)";
    }
    ctx.ui->out(line);
  }
  return 0;
}

int cmd_toolchain(CommandContext& ctx) {
  auto dirs = fs::cova_dirs();
  if (!dirs) {
    return report_error(dirs.error(), ctx);
  }

  switch (ctx.command()) {
    case CommandId::ToolchainList: {
      toolchain::DiscoveryOptions options;
      options.include_managed = true;
      auto discovered = toolchain::discover(options);
      if (!discovered) {
        return report_error(discovered.error(), ctx);
      }
      if (ctx.global.json) {
        json::Value root = json::Value::object();
        auto array = json::Value::array();
        for (const auto& descriptor : *discovered) {
          json::Value entry = json::Value::object();
          entry.set("id", descriptor.id);
          entry.set("family", descriptor.family);
          entry.set("version", descriptor.version.to_string());
          entry.set("source", std::string(toolchain::to_string(descriptor.source)));
          entry.set("host", descriptor.host.to_string());
          json::Value components = json::Value::object();
          for (const auto& component : descriptor.components) {
            components.set(component.role, fs::generic_string(component.path));
          }
          entry.set("components", std::move(components));
          array.push(std::move(entry));
        }
        root.set("toolchains", std::move(array));
        ctx.ui->out(root.dump(2));
        return 0;
      }
      if (discovered->empty()) {
        ctx.ui->warn("no toolchain was discovered on this host");
        ctx.ui->out("install a C++ compiler (clang or gcc), or run `cova toolchain install`");
        return 0;
      }
      ctx.ui->out("discovered toolchains:");
      for (const auto& descriptor : *discovered) {
        ctx.ui->out("  " + describe_toolchain_row(descriptor));
      }
      ctx.ui->out("");
      ctx.ui->out("managed root: " + fs::generic_string(dirs->toolchains));
      return 0;
    }
    case CommandId::ToolchainExplain: {
      return cmd_explain(ctx);
    }
    case CommandId::ToolchainInstall: {
      const std::string requested =
          ctx.positionals().empty() ? std::string() : ctx.positionals().front();
      toolchain::ToolchainInstallOptions options;
      options.registry_root = dirs->registry;
      if (auto registry = ctx.flags.get("registry"); registry && !registry->empty()) {
        options.registry_root = fs::absolute_normalized(*registry);
      }
      options.toolchains_root = dirs->toolchains;
      options.downloads_root = dirs->downloads;
      options.offline = ctx.global.offline_mode();
      options.allow_install = !ctx.global.no_install && !ctx.global.offline_mode();
      toolchain::ToolchainInstaller installer(options);

      if (requested.empty()) {
        auto available = installer.available();
        if (!available) {
          return report_error(available.error(), ctx);
        }
        if (available->empty()) {
          ctx.ui->info("no managed toolchains are available in the trusted registry");
          ctx.ui->out("registry: " + fs::generic_string(options.registry_root));
          ctx.ui->out("add a `[[toolchain]]` entry to its index.toml, or use a system compiler");
          return 1;
        }
        ctx.ui->info("available managed toolchains:");
        for (const auto& entry : *available) {
          ctx.ui->out("  " + entry.to_string());
        }
        ctx.ui->out("install one with `cova toolchain install <id>`");
        return 0;
      }

      auto installed = installer.install(requested);
      if (!installed) {
        return report_error(installed.error(), ctx);
      }
      ctx.ui->info("installed toolchain `" + installed->descriptor.label() + "` at " +
                   fs::generic_string(installed->root));
      ctx.ui->verbose("verified archive sha256 " + installed->entry.sha256);
      return 0;
    }
    case CommandId::ToolchainRemove: {
      if (ctx.positionals().empty()) {
        ctx.ui->error("`cova toolchain remove` requires a toolchain id");
        return 2;
      }
      const auto candidate = dirs->toolchains / ctx.positionals().front();
      if (!fs::is_within(candidate, dirs->toolchains) || !fs::exists(candidate)) {
        return report_error(Error::make("cova::toolchain::unknown",
                                        "no managed toolchain named `" + ctx.positionals().front() +
                                            "`"),
                            ctx);
      }
      auto removed = fs::remove_tree(candidate, dirs->toolchains);
      if (!removed) {
        return report_error(removed.error(), ctx);
      }
      ctx.ui->info("removed managed toolchain `" + ctx.positionals().front() + "`");
      return 0;
    }
    case CommandId::ToolchainGc: {
      // Only managed components are candidates; system toolchains are never touched.
      std::vector<fs::Path> managed;
      std::error_code ec;
      if (fs::is_directory(dirs->toolchains)) {
        for (auto it = stdfs::directory_iterator(dirs->toolchains, ec);
             it != stdfs::directory_iterator(); it.increment(ec)) {
          if (fs::is_directory(it->path())) {
            managed.push_back(it->path());
          }
        }
      }
      if (managed.empty()) {
        ctx.ui->info("no managed toolchains to reclaim");
        return 0;
      }
      ctx.ui->out("managed toolchains:");
      for (const auto& path : managed) {
        ctx.ui->out("  " + fs::generic_string(path));
      }
      if (ctx.flags.boolean("dry-run")) {
        ctx.ui->info("dry run: nothing was removed");
        return 0;
      }
      ctx.ui->warn(
          "automatic reclamation of managed toolchains is not enabled; nothing was removed");
      ctx.ui->out("run `cova toolchain remove <id>` to remove one explicitly");
      return 0;
    }
    default:
      ctx.ui->error("unknown toolchain subcommand");
      return 2;
  }
}

int cmd_sysroot(CommandContext& ctx) {
  auto dirs = fs::cova_dirs();
  if (!dirs) {
    return report_error(dirs.error(), ctx);
  }
  sysroot::ManagerOptions options;
  options.sysroots_root = dirs->sysroots;
  options.store_root = dirs->store;
  options.downloads_root = dirs->downloads;
  options.registry_root = dirs->registry;
  if (auto registry = ctx.flags.get("registry"); registry && !registry->empty()) {
    options.registry_root = fs::absolute_normalized(*registry);
  }
  options.offline = ctx.global.offline_mode();
  options.allow_install = !ctx.global.no_install && !ctx.global.offline_mode();
  options.interactive = ctx.ui->interactive();
  sysroot::SysrootManager manager(options);

  switch (ctx.command()) {
    case CommandId::SysrootList: {
      auto installed = manager.list();
      if (!installed) {
        return report_error(installed.error(), ctx);
      }
      if (ctx.global.json) {
        json::Value root = json::Value::object();
        auto array = json::Value::array();
        for (const auto& entry : *installed) {
          json::Value item = json::Value::object();
          item.set("id", entry.manifest.id);
          item.set("name", entry.manifest.name);
          item.set("target", entry.manifest.target.to_string());
          item.set("crt", std::string(platform::to_string(entry.manifest.crt)));
          item.set("root", fs::generic_string(entry.root));
          item.set("verified", entry.verified);
          array.push(std::move(item));
        }
        root.set("sysroots", std::move(array));
        ctx.ui->out(root.dump(2));
        return 0;
      }
      if (installed->empty()) {
        ctx.ui->info("no sysroots are installed");
        ctx.ui->out("sysroot root: " + fs::generic_string(dirs->sysroots));
        return 0;
      }
      ctx.ui->out("installed sysroots:");
      for (const auto& entry : *installed) {
        std::string line = "  " + entry.manifest.name;
        line += "  " + entry.manifest.target.to_string();
        line += "  " + fs::generic_string(entry.root);
        if (!entry.verified) {
          line += "  (unverified)";
        }
        ctx.ui->out(line);
      }
      return 0;
    }
    case CommandId::SysrootInstall: {
      std::string target_text;
      if (ctx.global.target) {
        target_text = *ctx.global.target;
      } else if (!ctx.positionals().empty()) {
        target_text = ctx.positionals().front();
      }
      if (target_text.empty()) {
        ctx.ui->error("`cova sysroot install` requires --target <triple>");
        return 2;
      }
      auto target = platform::resolve_target(target_text);
      if (!target) {
        return report_error(target.error(), ctx);
      }
      // A local archive is the only install source this build can verify.
      std::string name;
      if (ctx.positionals().size() > 1) {
        name = ctx.positionals()[1];
      }
      std::optional<fs::Path> archive;
      if (auto from = ctx.flags.get("from"); from && !from->empty()) {
        archive = fs::absolute_normalized(*from);
        if (!fs::is_regular_file(*archive)) {
          return report_error(Error::make("cova::sysroot::archive_missing",
                                          "archive " + fs::generic_string(*archive) +
                                              " does not exist"),
                              ctx);
        }
      }
      // With no explicit archive, consult the trusted registry index. An empty
      // name lets the index choose the entry whose target/CRT matches.
      auto installed = archive
                           ? manager.install_file(*target,
                                                  name.empty() ? target->to_short_string() : name,
                                                  archive)
                           : manager.install(*target, name);
      if (!installed) {
        return report_error(installed.error(), ctx);
      }
      ctx.ui->info("installed sysroot `" + installed->manifest.name + "` at " +
                   fs::generic_string(installed->root));
      if (!installed->manifest.sha256.empty()) {
        ctx.ui->verbose("verified archive sha256 " + installed->manifest.sha256);
      }
      return 0;
    }
    case CommandId::SysrootVerify: {
      auto installed = manager.list();
      if (!installed) {
        return report_error(installed.error(), ctx);
      }
      std::vector<sysroot::InstalledSysroot> candidates;
      if (!ctx.positionals().empty()) {
        auto found = manager.find(ctx.positionals().front());
        if (!found) {
          return report_error(found.error(), ctx);
        }
        candidates.push_back(std::move(*found));
      } else {
        candidates = std::move(*installed);
      }
      if (candidates.empty()) {
        ctx.ui->info("no sysroots to verify");
        return 0;
      }
      bool all_ok = true;
      for (const auto& entry : candidates) {
        sysroot::VerifyReport report;
        auto status = sysroot::verify(entry.root, entry.manifest, report);
        if (!status) {
          all_ok = false;
          ctx.ui->error("could not verify " + entry.manifest.name + ": " + status.error().message);
          continue;
        }
        ctx.ui->out(entry.manifest.name + ": " + (report.ok ? "ok" : "FAILED"));
        if (!report.ok) {
          all_ok = false;
          ctx.ui->out(report.to_string());
        }
      }
      return all_ok ? 0 : 1;
    }
    case CommandId::SysrootGc: {
      // Garbage collection over store entries no installed sysroot references.
      // Installed sysroots themselves are never candidates; remove those with
      // `cova sysroot remove`.
      auto plan = manager.plan_gc();
      if (!plan) {
        return report_error(plan.error(), ctx);
      }
      const bool dry_run = ctx.flags.boolean("dry-run");
      if (plan->removable.empty()) {
        ctx.ui->info("nothing to reclaim in the sysroot store");
        return 0;
      }
      if (ctx.global.json) {
        json::Value out = json::Value::object();
        out.set("dry_run", dry_run);
        out.set("reclaimable_bytes", static_cast<std::int64_t>(plan->reclaimable_bytes));
        auto entries = json::Value::array();
        for (const auto& path : plan->removable) {
          entries.push(fs::generic_string(path));
        }
        out.set("removable", std::move(entries));
        if (!dry_run) {
          auto applied = manager.apply_gc(*plan);
          if (!applied) {
            return report_error(applied.error(), ctx);
          }
          out.set("reclaimed_bytes", static_cast<std::int64_t>(plan->reclaimable_bytes));
        }
        ctx.ui->out(out.dump(2));
        return 0;
      }
      ctx.ui->out("unreferenced sysroot store entries:");
      for (const auto& path : plan->removable) {
        ctx.ui->out("  " + fs::generic_string(path));
      }
      if (dry_run) {
        ctx.ui->info("dry run: would reclaim " + human_bytes(plan->reclaimable_bytes));
        return 0;
      }
      if (!ctx.flags.boolean("yes")) {
        const std::string question =
            "Remove " + std::to_string(plan->removable.size()) +
            " unreferenced store entr" + (plan->removable.size() == 1 ? "y" : "ies") + " (" +
            human_bytes(plan->reclaimable_bytes) + ")?";
        if (!ctx.ui->confirm(question, false)) {
          ctx.ui->info("nothing was removed");
          return 0;
        }
      }
      auto applied = manager.apply_gc(*plan);
      if (!applied) {
        return report_error(applied.error(), ctx);
      }
      ctx.ui->info("reclaimed " + human_bytes(plan->reclaimable_bytes) +
                   " from the sysroot store");
      return 0;
    }
    default:
      ctx.ui->error("unknown sysroot subcommand");
      return 2;
  }
}

int cmd_doctor(CommandContext& ctx) {
  struct Check {
    std::string name;
    bool ok = true;
    bool fatal = false;
    std::string detail;
  };
  std::vector<Check> checks;

  // Data directory.
  auto dirs = fs::cova_dirs();
  checks.push_back(Check{"data directory", dirs.has_value(), true,
                         dirs ? fs::generic_string(dirs->root) : dirs.error().message});

  if (dirs) {
    auto layout = fs::ensure_layout(*dirs);
    checks.push_back(Check{"data layout", layout.has_value(), false,
                           layout ? "created" : layout.error().message});
  }

  // Project state.
  auto project = load_project(ctx);
  checks.push_back(Check{"cova.toml", project.has_value(), true,
                         project ? display_path(project->layout.manifest, ctx.cwd)
                                 : project.error().message});

  // Toolchains.
  std::vector<toolchain::ToolchainDescriptor> toolchains;
  if (auto discovered = toolchain::discover({})) {
    toolchains = *discovered;
  }
  checks.push_back(Check{"toolchain discovery", !toolchains.empty(), true,
                         toolchains.empty() ? "no compiler found on PATH"
                                            : std::to_string(toolchains.size()) +
                                                  " candidate(s) found"});

  // Per-family availability.
  for (const std::string family : {"clang", "gcc"}) {
    bool found = false;
    std::string version;
    for (const auto& descriptor : toolchains) {
      if (descriptor.family == family) {
        found = true;
        version = descriptor.version.to_string();
        break;
      }
    }
    checks.push_back(Check{family, found, false, found ? version : "not found on this host"});
  }

  // Formatter.
  formatter::ResolveOptions fmt_options;
  fmt_options.allow_install = false;
  auto formatter_descriptor = formatter::resolve_formatter(fmt_options);
  checks.push_back(Check{"clang-format", formatter_descriptor.has_value(), false,
                         formatter_descriptor
                             ? formatter_descriptor->version_text + " (" +
                                   formatter_descriptor->source + ")"
                             : "not found; `cova fmt` will not work"});

  // Trusted component registry. Missing is a warning, not a failure: system
  // toolchains and locally installed components still work.
  if (dirs) {
    const auto registry_index = dirs->registry / "index.toml";
    checks.push_back(
        Check{"component registry", fs::is_regular_file(registry_index), false,
              fs::is_regular_file(registry_index)
                  ? fs::generic_string(registry_index)
                  : "no index.toml; managed installs are unavailable, system components work"});
  }

  // Native target resolvability end to end.
  if (project) {
    auto config = effective_config(ctx);
    if (config) {
      auto target = resolve_target_from(ctx, *project);
      if (target) {
        auto request = make_resolution_request(ctx, *project, *target, *config);
        request.probe_capabilities = true;
        auto report = toolchain::resolve(request);
        checks.push_back(Check{"native resolution", report.resolved, true,
                               report.resolved
                                   ? report.toolchain.descriptor.label()
                                   : (report.failure.message.empty() ? "unresolved"
                                                                     : report.failure.message)});
        if (report.resolved) {
          const auto& caps = report.toolchain.descriptor.capabilities;
          checks.push_back(Check{"c++26 baseline", caps.cpp26_language && caps.cpp26_library,
                                 false,
                                 caps.cpp26_language && caps.cpp26_library
                                     ? "supported"
                                     : "the resolved compiler does not pass the C++26 probe"});
        }
      }
    }
  }

  // Runner availability for cross targets.
  auto config = effective_config(ctx);
  if (config) {
    const std::string qemu = config->user.runtime.qemu_prefix;
    if (!qemu.empty()) {
      checks.push_back(Check{"cross runner", fs::find_in_path(qemu).has_value(), false,
                             fs::find_in_path(qemu) ? fs::generic_string(*fs::find_in_path(qemu))
                                                    : qemu + " not found on PATH"});
    }
  }

  if (ctx.global.json) {
    json::Value root = json::Value::object();
    auto array = json::Value::array();
    for (const auto& check : checks) {
      json::Value entry = json::Value::object();
      entry.set("name", check.name);
      entry.set("ok", check.ok);
      entry.set("fatal", check.fatal);
      entry.set("detail", check.detail);
      array.push(std::move(entry));
    }
    root.set("checks", std::move(array));
    bool fatal_ok = true;
    for (const auto& check : checks) {
      if (check.fatal && !check.ok) {
        fatal_ok = false;
      }
    }
    root.set("ok", fatal_ok);
    ctx.ui->out(root.dump(2));
    return fatal_ok ? 0 : 1;
  }

  ctx.ui->out("cova doctor");
  std::size_t width = 0;
  for (const auto& check : checks) {
    width = std::max(width, check.name.size());
  }
  bool fatal_failure = false;
  bool any_warning = false;
  for (const auto& check : checks) {
    std::string line = "  ";
    line += check.ok ? "ok    " : (check.fatal ? "FAIL  " : "warn  ");
    line += check.name;
    line.append(width - check.name.size(), ' ');
    line += "  ";
    line += check.detail;
    ctx.ui->out(line);
    if (!check.ok) {
      if (check.fatal) {
        fatal_failure = true;
      } else {
        any_warning = true;
      }
    }
  }
  ctx.ui->out("");
  if (fatal_failure) {
    ctx.ui->error("the environment is not ready for Cova");
    return 1;
  }
  if (any_warning) {
    ctx.ui->info("the environment is usable, with warnings");
    return 0;
  }
  ctx.ui->info("the environment looks healthy");
  return 0;
}

}  // namespace cova::cli
