// Cova — Toolchain Resolver (PRD §7).
#include "toolchain/resolver/resolver.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/toml/toml.hpp"
#include "toolchain/discovery/discovery.hpp"
#include "toolchain/probe/probe.hpp"

namespace cova::toolchain {
namespace {

struct FoundSysroot {
  std::string id;
  std::string name;
  std::string target;
  platform::Crt crt = platform::Crt::Unknown;
  platform::LibCpp libcpp = platform::LibCpp::Unknown;
  stdfs::path path;
};

std::optional<platform::Crt> crt_from_text(std::string_view text) {
  const std::string value = str::to_lower(str::trim_view(text));
  if (value == "glibc") return platform::Crt::Glibc;
  if (value == "musl") return platform::Crt::Musl;
  if (value == "uclibc") return platform::Crt::Uclibc;
  if (value == "msvc") return platform::Crt::Msvc;
  if (value == "wasi") return platform::Crt::WasmWasi;
  if (value == "none") return platform::Crt::None;
  return std::nullopt;
}

std::optional<platform::LibCpp> libcpp_from_text(std::string_view text) {
  const std::string value = str::to_lower(str::trim_view(text));
  if (value == "libstdc++" || value == "libstdcxx") return platform::LibCpp::LibStdCpp;
  if (value == "libc++" || value == "libcxx") return platform::LibCpp::LibCpp;
  if (value == "msvc" || value == "msvc-stl") return platform::LibCpp::MsvcStl;
  if (value == "none") return platform::LibCpp::NoStd;
  return std::nullopt;
}

std::vector<FoundSysroot> scan_sysroots(const stdfs::path& root) {
  std::vector<FoundSysroot> out;
  if (root.empty() || !fs::is_directory(root)) {
    return out;
  }
  std::error_code ec;
  for (auto it = stdfs::directory_iterator(root, ec); it != stdfs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_directory()) {
      continue;
    }
    const auto manifest_path = it->path() / "sysroot.toml";
    if (!fs::is_regular_file(manifest_path)) {
      continue;
    }
    auto document = toml::parse_file(fs::generic_string(manifest_path));
    if (!document || !document->is_table()) {
      continue;
    }
    const auto& table = *document->as_table();
    auto read = [&](std::string_view key) -> std::string {
      auto entry = table.find(key);
      return entry != table.end() && entry->second.is_string() ? entry->second.as_string() : "";
    };
    FoundSysroot found;
    found.id = read("id");
    if (found.id.empty()) {
      found.id = it->path().filename().string();
    }
    found.name = read("name");
    found.target = read("target");
    if (found.target.empty()) {
      continue;
    }
    found.crt = crt_from_text(read("crt")).value_or(platform::Crt::Unknown);
    found.libcpp = libcpp_from_text(read("libcpp")).value_or(platform::LibCpp::Unknown);
    found.path = it->path() / "root";
    if (!fs::is_directory(found.path)) {
      found.path = it->path();
    }
    out.push_back(std::move(found));
  }
  std::sort(out.begin(), out.end(),
            [](const FoundSysroot& a, const FoundSysroot& b) { return a.id < b.id; });
  return out;
}

bool source_allowed(ToolchainSource source, Policy policy) {
  switch (policy) {
    case Policy::SystemOnly:
      return source == ToolchainSource::System || source == ToolchainSource::UserPath;
    case Policy::ManagedOnly:
      return source == ToolchainSource::Managed || source == ToolchainSource::Registry;
    case Policy::SystemFirst:
    case Policy::ManagedFirst:
      return true;
  }
  return true;
}

int source_rank(ToolchainSource source, Policy policy) {
  if (policy == Policy::ManagedFirst) {
    switch (source) {
      case ToolchainSource::Managed:
        return 0;
      case ToolchainSource::Registry:
        return 1;
      case ToolchainSource::UserPath:
        return 2;
      case ToolchainSource::System:
        return 3;
      case ToolchainSource::Unknown:
        return 4;
    }
  }
  switch (source) {
    case ToolchainSource::System:
      return 0;
    case ToolchainSource::UserPath:
      return 1;
    case ToolchainSource::Managed:
      return 2;
    case ToolchainSource::Registry:
      return 3;
    case ToolchainSource::Unknown:
      return 4;
  }
  return 4;
}

bool version_matches(const ToolchainDescriptor& descriptor, const ResolutionRequest& request) {
  if (!request.compiler_version.has_value() || request.compiler_version->empty()) {
    return true;
  }
  auto requirement = VersionRequirement::parse(*request.compiler_version);
  if (!requirement) {
    return false;
  }
  return requirement->matches(descriptor.version);
}

bool family_matches(const ToolchainDescriptor& descriptor, const ResolutionRequest& request) {
  if (!request.compiler_family.has_value() || request.compiler_family->empty()) {
    return true;
  }
  const std::string wanted = str::to_lower(*request.compiler_family);
  const std::string family = str::to_lower(descriptor.family);
  if (wanted == family) {
    return true;
  }
  return (wanted == "clang" && family == "clang-cl") ||
         (wanted == "gcc" && (family == "gcc" || family == "g++"));
}

platform::AbiSignature make_abi_signature(const ToolchainDescriptor& descriptor,
                                          const platform::Triple& target) {
  platform::AbiSignature signature;
  signature.triple = target;
  signature.abi = target.abi();
  signature.crt = target.crt();
  signature.libcpp = platform::default_libcpp_for(descriptor.family, target);
  signature.link_mode = platform::LinkMode::Default;
  signature.exceptions = true;
  signature.rtti = true;
  if (target.os == platform::Os::Windows) {
    signature.threads = platform::ThreadModel::Win32;
  } else if (target.os == platform::Os::Wasi) {
    signature.threads = platform::ThreadModel::Wasm;
  } else if (target.os == platform::Os::Bare) {
    signature.threads = platform::ThreadModel::Single;
  } else {
    signature.threads = platform::ThreadModel::Posix;
  }
  return signature;
}

bool is_cross_build(const platform::Triple& target) {
  return !target.is_native_like();
}

bool needs_sysroot(const platform::Triple& target) {
  if (!is_cross_build(target)) {
    return false;
  }
  return target.os == platform::Os::Linux || target.os == platform::Os::Android ||
         target.os == platform::Os::Wasi || target.os == platform::Os::Bare;
}

}  // namespace

std::vector<RankedCandidate> rank_candidates(const std::vector<ToolchainDescriptor>& candidates,
                                             const ResolutionRequest& request) {
  std::vector<RankedCandidate> ranked;
  for (const auto& descriptor : candidates) {
    if (!source_allowed(descriptor.source, request.policy)) {
      continue;
    }
    if (!family_matches(descriptor, request)) {
      continue;
    }
    RankedCandidate entry;
    entry.descriptor = descriptor;
    entry.rank = source_rank(descriptor.source, request.policy);
    // Prefer clang for cross compilation and newer versions for native builds.
    if (descriptor.family == "clang") {
      entry.rank -= 1;
    }
    entry.reason = std::string(to_string(descriptor.source)) + " candidate";
    ranked.push_back(std::move(entry));
  }
  std::sort(ranked.begin(), ranked.end(), [](const RankedCandidate& a, const RankedCandidate& b) {
    if (a.rank != b.rank) {
      return a.rank < b.rank;
    }
    if (a.descriptor.family != b.descriptor.family) {
      return a.descriptor.family < b.descriptor.family;
    }
    return b.descriptor.version < a.descriptor.version;
  });
  return ranked;
}

ResolutionReport resolve(const ResolutionRequest& request) {
  ResolutionReport report;
  std::vector<CandidateAttempt> attempts;

  std::vector<ToolchainDescriptor> candidates = request.candidates;
  if (candidates.empty()) {
    DiscoveryOptions options;
    options.include_system = request.policy != Policy::ManagedOnly;
    options.include_managed = request.policy != Policy::SystemOnly;
    options.include_user_path = true;
    if (request.compiler_family) {
      options.families.push_back(*request.compiler_family);
    }
    auto discovered = discover(options);
    if (discovered) {
      candidates = std::move(*discovered);
    }
    auto managed = discover_managed(request.toolchains_root);
    if (managed) {
      candidates.insert(candidates.end(), managed->begin(), managed->end());
    }
    report.decisions.push_back("discovered " + std::to_string(candidates.size()) +
                               " toolchain candidate(s)");
  }

  // A pinned toolchain may be a direct path.
  if (request.pinned_id && fs::is_regular_file(*request.pinned_id)) {
    if (auto described = describe_compiler(*request.pinned_id, ToolchainSource::UserPath)) {
      candidates.push_back(std::move(*described));
    }
  }

  auto ranked = rank_candidates(candidates, request);
  auto sysroots = scan_sysroots(request.sysroots_root);

  if (ranked.empty()) {
    report.failure = make_error("cova::toolchain::unresolved", "unable to resolve a toolchain");
    report.failure.note("target", request.target.to_string());
    report.failure.line("no compatible toolchain candidates were found on this host");
    report.failure.line("install a C++ compiler (clang or gcc) and retry");
    for (const auto& decision : report.decisions) {
      report.failure.line(decision);
    }
    return report;
  }

  for (const auto& candidate : ranked) {
    CandidateAttempt attempt;
    attempt.label = candidate.descriptor.label();
    const auto& descriptor = candidate.descriptor;

    if (request.pinned_id && !request.pinned_id->empty() &&
        *request.pinned_id != descriptor.id && *request.pinned_id != descriptor.label() &&
        !fs::is_regular_file(*request.pinned_id)) {
      attempt.reason = platform::RejectionReason::VersionMismatch;
      attempt.missing.push_back("toolchain `" + *request.pinned_id + "` is not installed");
      attempts.push_back(attempt);
      report.decisions.push_back(attempt.label + ": rejected (not the pinned toolchain)");
      continue;
    }
    if (request.pinned_content_hash && !request.pinned_content_hash->empty() &&
        !descriptor.content_hash.empty() &&
        *request.pinned_content_hash != descriptor.content_hash) {
      attempt.reason = platform::RejectionReason::VersionMismatch;
      attempt.missing.push_back("content hash does not match the lockfile");
      attempts.push_back(attempt);
      report.decisions.push_back(attempt.label + ": rejected (content hash mismatch)");
      continue;
    }
    if (!version_matches(descriptor, request)) {
      attempt.reason = platform::RejectionReason::VersionMismatch;
      attempt.missing.push_back("compiler version " +
                                request.compiler_version.value_or(""));
      attempts.push_back(attempt);
      report.decisions.push_back(attempt.label + ": rejected (version requirement)");
      continue;
    }
    attempt.matched_version = true;

    const bool cross = is_cross_build(request.target);
    if (cross && descriptor.family != "clang" && descriptor.family != "clang-cl" &&
        descriptor.family != "gcc") {
      attempt.reason = platform::RejectionReason::TargetUnsupported;
      attempt.missing.push_back("a cross-capable compiler driver");
      attempts.push_back(attempt);
      report.decisions.push_back(attempt.label + ": rejected (not cross-capable)");
      continue;
    }
    attempt.matched_target = true;

    // Locate a sysroot before probing so the probe uses the real target headers
    // and libraries rather than the host's.
    std::optional<FoundSysroot> sysroot;
    if (needs_sysroot(request.target)) {
      for (const auto& entry : sysroots) {
        auto parsed = platform::Triple::parse(entry.target);
        if (!parsed || parsed->arch != request.target.arch || parsed->os != request.target.os) {
          continue;
        }
        if (parsed->env != request.target.env && parsed->env != platform::Env::Unknown) {
          continue;
        }
        if (entry.crt != platform::Crt::Unknown && entry.crt != request.target.crt()) {
          continue;
        }
        sysroot = entry;
        break;
      }
      if (!sysroot) {
        attempt.reason = request.offline
                             ? platform::RejectionReason::OfflineRestriction
                             : (request.allow_install
                                    ? platform::RejectionReason::DownloadUnavailable
                                    : platform::RejectionReason::SysrootMissing);
        attempt.missing.push_back("a compatible " + request.target.to_string() + " sysroot");
        attempts.push_back(attempt);
        report.decisions.push_back(attempt.label + ": rejected (no compatible sysroot)");
        continue;
      }
    }

    ToolchainDescriptor probed = descriptor;
    if (sysroot) {
      probed.default_sysroot = sysroot->path;
      probed.required_sysroot_target = sysroot->target;
    }

    platform::CandidateEnvironment environment = probed.to_candidate(request.target);
    if (request.probe_capabilities) {
      ProbeReport probe_report;
      bool have_probe = false;
      if (!request.cache_root.empty()) {
        auto cached = load_cached_probe(request.cache_root, probed);
        if (cached) {
          probe_report = std::move(*cached);
          have_probe = true;
          report.decisions.push_back(attempt.label + ": using cached capability probe");
        }
      }
      if (!have_probe) {
        ProbeOptions probe_options;
        probe_options.target = request.target;
        probe_options.required = {"cpp", "link"};
        probe_report = probe(probed, probe_options).value_or(ProbeReport{});
        if (!request.cache_root.empty()) {
          auto stored = store_cached_probe(request.cache_root, probed, probe_report);
          (void)stored;
        }
        report.decisions.push_back(attempt.label + ": probed capabilities");
      }
      probed.capabilities = probe_report.capabilities;
      if (!probe_report.succeeded || !probed.capabilities.cpp_compilation ||
          !probed.capabilities.executable_link) {
        attempt.reason = platform::RejectionReason::NotProbed;
        attempt.missing.push_back("C++ compilation and executable linking for " +
                                  request.target.to_string());
        if (!probe_report.failures.empty()) {
          attempt.missing.push_back(probe_report.failures.front());
        }
        attempts.push_back(attempt);
        report.decisions.push_back(attempt.label + ": rejected (capability probe failed)");
        continue;
      }
      environment = probed.to_candidate(request.target);
    }
    attempt.probe_passed = true;
    environment.sysroot_present = sysroot.has_value() || !probed.default_sysroot.empty();
    environment.sysroot_id = sysroot ? sysroot->id : "";

    auto verdict = platform::evaluate(environment, request.constraints);
    if (!verdict.satisfied) {
      attempt.reason = verdict.reason == platform::RejectionReason::None
                           ? platform::RejectionReason::ConstraintUnsatisfied
                           : verdict.reason;
      for (const auto& constraint : verdict.failed) {
        attempt.missing.push_back(constraint.to_string());
      }
      attempts.push_back(attempt);
      report.decisions.push_back(attempt.label + ": rejected (" + verdict.explanation + ")");
      continue;
    }
    attempt.matched_abi = true;

    ResolvedToolchain resolved;
    resolved.descriptor = probed;
    resolved.target = request.target;
    resolved.abi_signature = make_abi_signature(probed, request.target);
    if (sysroot) {
      resolved.sysroot_id = sysroot->id;
      resolved.sysroot_path = sysroot->path;
    }
    resolved.environment = environment;
    resolved.decisions = report.decisions;
    resolved.fingerprint = resolved.fingerprint_input();
    report.decisions.push_back("selected " + resolved.descriptor.label());
    report.toolchain = std::move(resolved);
    report.resolved = true;
    report.rejected.clear();
    for (const auto& rejected : attempts) {
      report.rejected.push_back(RejectedCandidate{
          rejected.label, rejected.reason, str::join(rejected.missing, ", ")});
    }
    return report;
  }

  // Nothing survived.
  for (const auto& rejected : attempts) {
    report.rejected.push_back(RejectedCandidate{
        rejected.label, rejected.reason, str::join(rejected.missing, ", ")});
  }
  report.failure = build_failure_diagnostic(request, attempts);
  for (const auto& decision : report.decisions) {
    report.failure.line(decision);
  }
  return report;
}

Diagnostic build_failure_diagnostic(const ResolutionRequest& request,
                                    const std::vector<CandidateAttempt>& attempts) {
  Diagnostic diagnostic =
      make_error("cova::toolchain::unresolved", "unable to resolve a toolchain");
  diagnostic.note("target", request.target.to_string());

  std::string required;
  if (request.compiler_family) {
    required += *request.compiler_family;
  } else {
    required += "any C++ compiler";
  }
  if (request.compiler_version && !request.compiler_version->empty()) {
    required += " " + *request.compiler_version;
  }
  required += ", C++ compilation, executable linking";
  diagnostic.note("required", required);

  std::string found;
  for (const auto& attempt : attempts) {
    if (!found.empty()) {
      found += "; ";
    }
    found += attempt.label;
  }
  diagnostic.note("found", found.empty() ? "no candidates" : found);

  std::string missing;
  for (const auto& attempt : attempts) {
    for (const auto& item : attempt.missing) {
      if (missing.find(item) == std::string::npos) {
        if (!missing.empty()) {
          missing += "; ";
        }
        missing += item;
      }
    }
  }
  diagnostic.note("missing", missing.empty() ? "a compatible toolchain" : missing);
  if (request.offline) {
    diagnostic.line("offline mode is active: no component may be downloaded");
  }
  if (!request.allow_install) {
    diagnostic.line("installation is disabled: only local components may be used");
  }
  diagnostic.line("suggested action: install a C++ compiler (clang or gcc), or provide a compatible sysroot with `cova sysroot install`");
  return diagnostic;
}

std::string toolchain_lock_entry(const ResolvedToolchain& resolved) {
  toml::Table table;
  table.emplace("target", toml::Value(resolved.target.to_string()));
  table.emplace("toolchain-id", toml::Value(resolved.descriptor.id));
  table.emplace("toolchain-family", toml::Value(resolved.descriptor.family));
  table.emplace("toolchain-version", toml::Value(resolved.descriptor.version.to_string()));
  table.emplace("content-hash", toml::Value(resolved.descriptor.content_hash));
  table.emplace("source", toml::Value(std::string(to_string(resolved.descriptor.source))));
  table.emplace("host", toml::Value(resolved.descriptor.host.to_string()));
  if (!resolved.sysroot_id.empty()) {
    table.emplace("sysroot-id", toml::Value(resolved.sysroot_id));
  }
  table.emplace("abi-key", toml::Value(resolved.abi_signature.fingerprint_key()));
  return toml::write(table);
}

}  // namespace cova::toolchain
