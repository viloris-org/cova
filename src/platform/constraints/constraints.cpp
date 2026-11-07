// Cova — target constraints and compatibility judgement (PRD §6.3, TGT-003/005).
#include "platform/constraints/constraints.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "core/toml/toml.hpp"

namespace cova::platform {
namespace {

std::string lower_trim(std::string_view text) { return str::to_lower(str::trim_view(text)); }

std::optional<Crt> crt_from_text(std::string_view text) {
  const std::string value = lower_trim(text);
  if (value == "glibc" || value == "gnu" || value == "libc") {
    return Crt::Glibc;
  }
  if (value == "musl") {
    return Crt::Musl;
  }
  if (value == "uclibc") {
    return Crt::Uclibc;
  }
  if (value == "msvc") {
    return Crt::Msvc;
  }
  if (value == "wasi" || value == "wasm") {
    return Crt::WasmWasi;
  }
  if (value == "none" || value == "freestanding") {
    return Crt::None;
  }
  return std::nullopt;
}

std::optional<LibCpp> libcpp_from_text(std::string_view text) {
  const std::string value = lower_trim(text);
  if (value == "libstdc++" || value == "libstdcxx" || value == "stdc++") {
    return LibCpp::LibStdCpp;
  }
  if (value == "libc++" || value == "libcxx") {
    return LibCpp::LibCpp;
  }
  if (value == "msvc" || value == "msvc-stl" || value == "msvcstl") {
    return LibCpp::MsvcStl;
  }
  if (value == "none" || value == "nostd" || value == "no-std") {
    return LibCpp::NoStd;
  }
  return std::nullopt;
}

std::optional<LinkMode> link_mode_from_text(std::string_view text) {
  const std::string value = lower_trim(text);
  if (value == "static") {
    return LinkMode::Static;
  }
  if (value == "dynamic" || value == "shared") {
    return LinkMode::Dynamic;
  }
  if (value == "default" || value == "auto") {
    return LinkMode::Default;
  }
  return std::nullopt;
}

std::optional<ThreadModel> thread_model_from_text(std::string_view text) {
  const std::string value = lower_trim(text);
  if (value == "posix" || value == "pthread" || value == "pthreads") {
    return ThreadModel::Posix;
  }
  if (value == "win32" || value == "windows") {
    return ThreadModel::Win32;
  }
  if (value == "single" || value == "none" || value == "no-threads") {
    return ThreadModel::Single;
  }
  if (value == "wasm") {
    return ThreadModel::Wasm;
  }
  return std::nullopt;
}

std::optional<Abi> abi_from_text(std::string_view text) {
  const std::string value = lower_trim(text);
  if (value == "sysv" || value == "sysv64") {
    return Abi::SysV;
  }
  if (value == "msvc" || value == "msabi") {
    return Abi::Msvc;
  }
  if (value == "gnueabi") {
    return Abi::Gnueabi;
  }
  if (value == "gnueabihf" || value == "eabihf") {
    return Abi::Gnueabihf;
  }
  if (value == "musl") {
    return Abi::Musl;
  }
  if (value == "wasm" || value == "wasm32") {
    return Abi::Wasm;
  }
  if (value == "eabi") {
    return Abi::Eabi;
  }
  if (value == "macho") {
    return Abi::MachO;
  }
  return std::nullopt;
}

Constraint make_constraint(ConstraintKind kind, std::string key, std::string value,
                           bool required, const std::string& origin) {
  Constraint constraint;
  constraint.kind = kind;
  constraint.key = std::move(key);
  constraint.value = std::move(value);
  constraint.required = required;
  constraint.origin = origin;
  return constraint;
}

std::optional<bool> parse_bool_text(std::string_view text) {
  const std::string value = lower_trim(text);
  if (value == "true" || value == "on" || value == "yes" || value == "1") {
    return true;
  }
  if (value == "false" || value == "off" || value == "no" || value == "0") {
    return false;
  }
  return std::nullopt;
}

// Returns true when the constraint is satisfied. `explanation` receives a
// human-readable reason when it is not.
bool check_one(const CandidateEnvironment& candidate, const Constraint& constraint,
               std::string& explanation) {
  switch (constraint.kind) {
    case ConstraintKind::Arch: {
      auto expected = arch_from_string(constraint.value);
      if (!expected) {
        explanation = "unknown architecture `" + constraint.value + "` in constraint";
        return false;
      }
      if (candidate.triple.arch != *expected) {
        explanation = "requires architecture " + std::string(to_string(*expected)) + ", found " +
                      std::string(to_string(candidate.triple.arch));
        return false;
      }
      return true;
    }
    case ConstraintKind::OsVersion: {
      auto expected = OsVersion::parse(constraint.value);
      if (!expected) {
        explanation = "invalid OS version `" + constraint.value + "`";
        return false;
      }
      if (!candidate.os_version.has_value()) {
        explanation = "the candidate does not declare an OS version (requires " +
                      expected->to_string() + ")";
        return false;
      }
      if (*candidate.os_version < *expected) {
        explanation = "requires OS version >= " + expected->to_string() + ", found " +
                      candidate.os_version->to_string();
        return false;
      }
      return true;
    }
    case ConstraintKind::Crt: {
      auto expected = crt_from_text(constraint.value);
      if (!expected) {
        explanation = "unknown C runtime `" + constraint.value + "`";
        return false;
      }
      if (candidate.crt != *expected) {
        explanation = "requires C runtime " + std::string(platform::to_string(*expected)) +
                      ", candidate provides " + std::string(platform::to_string(candidate.crt));
        return false;
      }
      return true;
    }
    case ConstraintKind::LibCpp: {
      auto expected = libcpp_from_text(constraint.value);
      if (!expected) {
        explanation = "unknown C++ standard library `" + constraint.value + "`";
        return false;
      }
      if (candidate.libcpp != *expected) {
        explanation = "requires " + std::string(platform::to_string(*expected)) +
                      ", candidate provides " +
                      std::string(platform::to_string(candidate.libcpp));
        return false;
      }
      return true;
    }
    case ConstraintKind::CpuMicroArch: {
      const std::string wanted = lower_trim(constraint.value);
      if (wanted.empty() || wanted == "default" || wanted == "generic" || wanted == "native") {
        return true;
      }
      const std::string arch = lower_trim(to_string(candidate.triple.arch));
      if (arch != wanted) {
        explanation = "requires CPU `" + wanted + "`, target is " + arch;
        return false;
      }
      return true;
    }
    case ConstraintKind::CpuFeature: {
      const std::string wanted = lower_trim(constraint.value);
      const bool found = std::any_of(candidate.cpu_features.begin(), candidate.cpu_features.end(),
                                     [&](const std::string& feature) {
                                       return lower_trim(feature) == wanted;
                                     });
      if (!found) {
        explanation = "requires CPU feature `" + wanted + "`, which the toolchain did not report";
        return false;
      }
      return true;
    }
    case ConstraintKind::LinkMode: {
      auto expected = link_mode_from_text(constraint.value);
      if (!expected) {
        explanation = "unknown link mode `" + constraint.value + "`";
        return false;
      }
      if (candidate.link_mode != *expected) {
        explanation = "requires " + std::string(platform::to_string(*expected)) + " linking";
        return false;
      }
      return true;
    }
    case ConstraintKind::Exceptions: {
      auto expected = parse_bool_text(constraint.value);
      if (!expected) {
        explanation = "invalid exceptions value `" + constraint.value + "`";
        return false;
      }
      if (candidate.supports_exceptions != *expected) {
        explanation = std::string("requires exceptions ") + (*expected ? "on" : "off");
        return false;
      }
      return true;
    }
    case ConstraintKind::Rtti: {
      auto expected = parse_bool_text(constraint.value);
      if (!expected) {
        explanation = "invalid rtti value `" + constraint.value + "`";
        return false;
      }
      if (candidate.supports_rtti != *expected) {
        explanation = std::string("requires RTTI ") + (*expected ? "on" : "off");
        return false;
      }
      return true;
    }
    case ConstraintKind::ThreadModel: {
      auto expected = thread_model_from_text(constraint.value);
      if (!expected) {
        explanation = "unknown threading model `" + constraint.value + "`";
        return false;
      }
      if (candidate.threads != *expected) {
        explanation = "requires threading model " + std::string(platform::to_string(*expected));
        return false;
      }
      return true;
    }
    case ConstraintKind::SdkVersion: {
      if (constraint.key == "sysroot") {
        if (!candidate.sysroot_present) {
          explanation = "an explicit sysroot was requested but none was resolved";
          return false;
        }
        if (!candidate.sysroot_id.empty() && !constraint.value.empty() &&
            candidate.sysroot_id != constraint.value &&
            candidate.sysroot_id.find(constraint.value) == std::string::npos) {
          explanation = "requested sysroot `" + constraint.value + "`, resolved `" +
                        candidate.sysroot_id + "`";
          return false;
        }
        return true;
      }
      auto expected = Version::parse(constraint.value);
      if (!expected) {
        explanation = "invalid SDK version `" + constraint.value + "`";
        return false;
      }
      if (!candidate.sdk_version.has_value()) {
        explanation = "the candidate does not declare an SDK version";
        return false;
      }
      if (*candidate.sdk_version < *expected) {
        explanation = "requires SDK >= " + expected->to_string() + ", found " +
                      candidate.sdk_version->to_string();
        return false;
      }
      return true;
    }
    case ConstraintKind::Abi: {
      auto expected = abi_from_text(constraint.value);
      if (!expected) {
        explanation = "unknown ABI `" + constraint.value + "`";
        return false;
      }
      if (candidate.abi != *expected) {
        explanation = "requires ABI " + std::string(platform::to_string(*expected)) + ", found " +
                      std::string(platform::to_string(candidate.abi));
        return false;
      }
      return true;
    }
    case ConstraintKind::Unknown:
      explanation = "unknown constraint kind";
      return false;
  }
  explanation = "unknown constraint";
  return false;
}

}  // namespace

std::string_view to_string(ConstraintKind kind) {
  switch (kind) {
    case ConstraintKind::Arch:
      return "arch";
    case ConstraintKind::OsVersion:
      return "os-version";
    case ConstraintKind::Crt:
      return "crt";
    case ConstraintKind::LibCpp:
      return "libcpp";
    case ConstraintKind::CpuMicroArch:
      return "cpu-microarch";
    case ConstraintKind::CpuFeature:
      return "cpu-feature";
    case ConstraintKind::LinkMode:
      return "link-mode";
    case ConstraintKind::Exceptions:
      return "exceptions";
    case ConstraintKind::Rtti:
      return "rtti";
    case ConstraintKind::ThreadModel:
      return "thread-model";
    case ConstraintKind::SdkVersion:
      return "sdk-version";
    case ConstraintKind::Abi:
      return "abi";
    case ConstraintKind::Unknown:
      break;
  }
  return "unknown";
}

std::string Constraint::to_string() const {
  std::string out;
  out += platform::to_string(kind);
  out += "=";
  out += value;
  if (!required) {
    out += " (preference)";
  }
  return out;
}

Result<std::vector<Constraint>> constraints_from_table(const void* toml_table,
                                                      std::string origin) {
  std::vector<Constraint> constraints;
  if (toml_table == nullptr) {
    return constraints;
  }
  const auto* table = static_cast<const toml::Table*>(toml_table);
  if (table == nullptr) {
    return constraints;
  }
  auto add_text = [&](std::string_view key, ConstraintKind kind) {
    auto it = table->find(key);
    if (it == table->end() || !it->second.is_string()) {
      return;
    }
    std::string value = it->second.as_string();
    if (!value.empty()) {
      constraints.push_back(make_constraint(kind, std::string(key), std::move(value), true, origin));
    }
  };

  add_text("cpu-arch", ConstraintKind::Arch);
  add_text("cpu", ConstraintKind::CpuMicroArch);
  add_text("link", ConstraintKind::LinkMode);
  add_text("exceptions", ConstraintKind::Exceptions);
  add_text("rtti", ConstraintKind::Rtti);
  add_text("threads", ConstraintKind::ThreadModel);
  add_text("crt", ConstraintKind::Crt);
  add_text("libcpp", ConstraintKind::LibCpp);
  add_text("os-version", ConstraintKind::OsVersion);
  add_text("sdk", ConstraintKind::SdkVersion);
  add_text("abi", ConstraintKind::Abi);

  // Boolean spellings are also accepted for exceptions/rtti.
  for (auto [key, kind] : {std::pair<std::string_view, ConstraintKind>{"exceptions",
                                                                       ConstraintKind::Exceptions},
                           {"rtti", ConstraintKind::Rtti}}) {
    auto it = table->find(key);
    if (it == table->end() || !it->second.is_bool()) {
      continue;
    }
    constraints.push_back(make_constraint(kind, std::string(key),
                                          it->second.as_bool() ? "true" : "false", true, origin));
  }

  if (auto it = table->find("features"); it != table->end()) {
    for (const auto& feature : it->second.as_string_array()) {
      if (!feature.empty()) {
        constraints.push_back(
            make_constraint(ConstraintKind::CpuFeature, "feature", feature, true, origin));
      }
    }
  }
  // Keep the list deterministic: table order in TOML is not guaranteed to be
  // meaningful after parsing into a map, but sorting makes cache keys stable.
  std::sort(constraints.begin(), constraints.end(),
            [](const Constraint& a, const Constraint& b) {
              if (a.kind != b.kind) {
                return a.kind < b.kind;
              }
              if (a.key != b.key) {
                return a.key < b.key;
              }
              return a.value < b.value;
            });
  return constraints;
}

std::string_view to_string(RejectionReason reason) {
  switch (reason) {
    case RejectionReason::VersionMismatch:
      return "version-mismatch";
    case RejectionReason::TargetUnsupported:
      return "target-unsupported";
    case RejectionReason::SysrootMissing:
      return "sysroot-missing";
    case RejectionReason::RuntimeIncompatible:
      return "runtime-incompatible";
    case RejectionReason::DownloadUnavailable:
      return "download-unavailable";
    case RejectionReason::OfflineRestriction:
      return "offline-restriction";
    case RejectionReason::RegistryVerificationFailed:
      return "registry-verification-failed";
    case RejectionReason::ConstraintUnsatisfied:
      return "constraint-unsatisfied";
    case RejectionReason::NotProbed:
      return "not-probed";
    case RejectionReason::None:
      break;
  }
  return "none";
}

ConstraintVerdict evaluate(const CandidateEnvironment& candidate,
                           const std::vector<Constraint>& constraints) {
  ConstraintVerdict verdict;
  for (const auto& constraint : constraints) {
    std::string explanation;
    const bool satisfied = check_one(candidate, constraint, explanation);
    if (satisfied) {
      continue;
    }
    if (constraint.required) {
      verdict.failed.push_back(constraint);
      if (verdict.reason == RejectionReason::None) {
        verdict.reason = RejectionReason::ConstraintUnsatisfied;
      }
      if (verdict.explanation.empty()) {
        verdict.explanation = explanation;
      } else {
        verdict.explanation += "; " + explanation;
      }
    } else {
      verdict.unmet_preferences.push_back(constraint);
    }
  }
  verdict.satisfied = verdict.failed.empty();
  return verdict;
}

SysrootCompatResult check_sysroot_compat(const Triple& target,
                                         const std::string& sysroot_target_triple,
                                         Crt sysroot_crt, LinkMode link_mode,
                                         std::optional<Version> sysroot_min_os) {
  SysrootCompatResult result;
  auto parsed = Triple::parse(sysroot_target_triple);
  if (!parsed) {
    result.reason = RejectionReason::SysrootMissing;
    result.explanation = "sysroot manifest has an invalid target triple `" +
                         sysroot_target_triple + "`";
    return result;
  }
  if (parsed->arch != target.arch) {
    result.reason = RejectionReason::TargetUnsupported;
    result.explanation = "sysroot targets " + std::string(to_string(parsed->arch)) +
                         ", build targets " + std::string(to_string(target.arch));
    return result;
  }
  if (parsed->os != target.os) {
    result.reason = RejectionReason::RuntimeIncompatible;
    result.explanation = "sysroot targets " + std::string(to_string(parsed->os)) +
                         ", build targets " + std::string(to_string(target.os));
    return result;
  }
  if (sysroot_crt != Crt::Unknown && target.crt() != Crt::Unknown &&
      sysroot_crt != target.crt()) {
    result.reason = RejectionReason::RuntimeIncompatible;
    result.explanation = "sysroot provides " + std::string(to_string(sysroot_crt)) +
                         ", target requires " + std::string(to_string(target.crt()));
    return result;
  }
  if (link_mode == LinkMode::Static && target.os == Os::Windows &&
      sysroot_crt == Crt::Msvc) {
    result.reason = RejectionReason::RuntimeIncompatible;
    result.explanation = "static MSVC linking is not supported by this build";
    return result;
  }
  if (sysroot_min_os.has_value() && !target.os_version.empty()) {
    auto target_version = Version::parse(target.os_version);
    if (target_version && *target_version < *sysroot_min_os) {
      result.reason = RejectionReason::RuntimeIncompatible;
      result.explanation = "target OS version " + target.os_version + " is older than the sysroot's " +
                           sysroot_min_os->to_string();
      return result;
    }
  }
  result.compatible = true;
  result.explanation = "sysroot target, runtime and OS floor are compatible";
  return result;
}

bool needs_glibc_version_floor(const Triple& triple) {
  return triple.os == Os::Linux && triple.crt() == Crt::Glibc;
}

std::string describe_environment(const CandidateEnvironment& env) {
  std::string out;
  if (!env.compiler_family.empty()) {
    out += env.compiler_family;
    if (env.compiler_version.has_value()) {
      out += " " + env.compiler_version->to_string();
    }
    out += " ";
  }
  out += "targeting " + env.triple.to_string();
  out += " (abi " + std::string(to_string(env.abi)) + ", crt " +
         std::string(to_string(env.crt)) + ", c++ " + std::string(to_string(env.libcpp)) +
         ", " + std::string(to_string(env.link_mode)) + " link";
  if (env.threads != ThreadModel::Unknown) {
    out += ", " + std::string(to_string(env.threads)) + " threads";
  }
  out += ")";
  if (env.sysroot_present && !env.sysroot_id.empty()) {
    out += ", sysroot " + env.sysroot_id;
  }
  return out;
}

}  // namespace cova::platform
