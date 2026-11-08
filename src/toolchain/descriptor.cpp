// Cova — toolchain descriptors (PRD §7.5, §7.6).
#include "toolchain/descriptor.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"

namespace cova::toolchain {

std::string_view to_string(ToolchainSource source) {
  switch (source) {
    case ToolchainSource::System:
      return "system";
    case ToolchainSource::Managed:
      return "managed";
    case ToolchainSource::UserPath:
      return "user-path";
    case ToolchainSource::Registry:
      return "registry";
    case ToolchainSource::Unknown:
      break;
  }
  return "unknown";
}

std::string_view to_string(Policy policy) {
  switch (policy) {
    case Policy::SystemFirst:
      return "system-first";
    case Policy::ManagedFirst:
      return "managed-first";
    case Policy::SystemOnly:
      return "system-only";
    case Policy::ManagedOnly:
      return "managed-only";
  }
  return "system-first";
}

std::optional<Policy> policy_from_string(std::string_view text) {
  const std::string value = str::to_lower(str::trim_view(text));
  if (value == "system-first" || value == "system_first") {
    return Policy::SystemFirst;
  }
  if (value == "managed-first" || value == "managed_first") {
    return Policy::ManagedFirst;
  }
  if (value == "system-only" || value == "system_only") {
    return Policy::SystemOnly;
  }
  if (value == "managed-only" || value == "managed_only") {
    return Policy::ManagedOnly;
  }
  return std::nullopt;
}

std::string ToolchainCapabilities::to_string() const {
  std::vector<std::string> enabled;
  if (c_compilation) enabled.emplace_back("c");
  if (cpp_compilation) enabled.emplace_back("c++");
  if (preprocess) enabled.emplace_back("preprocess");
  if (static_library) enabled.emplace_back("archive");
  if (shared_library) enabled.emplace_back("shared");
  if (executable_link) enabled.emplace_back("link");
  if (cpp26_language) enabled.emplace_back("c++26-language");
  if (cpp26_library) enabled.emplace_back("c++26-library");
  if (stdlib_headers) enabled.emplace_back("stdlib-headers");
  if (lto) enabled.emplace_back("lto");
  if (modules) enabled.emplace_back("modules");
  if (sanitizers) enabled.emplace_back("sanitizers");
  if (coverage) enabled.emplace_back("coverage");
  if (debug_info) enabled.emplace_back("debug-info");
  if (threads_available) enabled.emplace_back("threads");
  if (cpp_modules_scan) enabled.emplace_back("module-scan");
  std::string out = str::join(enabled, ", ");
  if (!probed_features.empty()) {
    if (!out.empty()) {
      out += ", ";
    }
    out += "features: " + str::join(probed_features, ",");
  }
  return out;
}

bool ToolchainDescriptor::has_component(std::string_view role) const {
  return component(role) != nullptr;
}

const ToolComponent* ToolchainDescriptor::component(std::string_view role) const {
  for (const auto& entry : components) {
    if (entry.role == role) {
      return &entry;
    }
  }
  return nullptr;
}

stdfs::path ToolchainDescriptor::component_path(std::string_view role) const {
  const ToolComponent* entry = component(role);
  return entry == nullptr ? stdfs::path{} : entry->path;
}

std::string ToolchainDescriptor::label() const {
  std::string out = family.empty() ? "unknown" : family;
  if (!version.to_string().empty()) {
    out += " " + version.to_string();
  }
  out += " (";
  out += cova::toolchain::to_string(source);
  out += ")";
  return out;
}

platform::CandidateEnvironment ToolchainDescriptor::to_candidate(
    const platform::Triple& target) const {
  platform::CandidateEnvironment candidate;
  candidate.triple = target;
  candidate.compiler_family = family;
  candidate.compiler_version = version;
  candidate.os_version = std::nullopt;
  candidate.crt = crt != platform::Crt::Unknown ? crt : target.crt();
  candidate.libcpp = libcpp != platform::LibCpp::Unknown ? libcpp : platform::default_libcpp_for(family, target);
  candidate.abi = abi != platform::Abi::Unknown ? abi : target.abi();
  candidate.supports_exceptions = true;
  candidate.supports_rtti = true;
  if (target.os == platform::Os::Windows) {
    candidate.threads = platform::ThreadModel::Win32;
  } else if (target.os == platform::Os::Wasi) {
    candidate.threads = platform::ThreadModel::Wasm;
  } else if (target.os == platform::Os::Bare) {
    candidate.threads = platform::ThreadModel::Single;
  } else {
    candidate.threads = platform::ThreadModel::Posix;
  }
  candidate.sysroot_id = required_sysroot_target.value_or("");
  candidate.sysroot_present = !default_sysroot.empty();
  return candidate;
}

std::string ToolchainDescriptor::compute_fingerprint() const {
  cova::Fingerprint fingerprint;
  fingerprint.add(id);
  fingerprint.add(family);
  fingerprint.add(version.to_string());
  fingerprint.add(cova::toolchain::to_string(source));
  fingerprint.add(host.to_string());
  std::vector<std::string> targets = supported_targets;
  std::sort(targets.begin(), targets.end());
  for (const auto& target : targets) {
    fingerprint.add(target);
  }
  std::vector<const ToolComponent*> sorted;
  sorted.reserve(components.size());
  for (const auto& entry : components) {
    sorted.push_back(&entry);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const ToolComponent* a, const ToolComponent* b) { return a->role < b->role; });
  for (const auto* entry : sorted) {
    fingerprint.add(entry->role);
    fingerprint.add(fs::generic_string(entry->path));
    fingerprint.add(entry->sha256);
  }
  fingerprint.add(fs::generic_string(resource_dir));
  fingerprint.add(fs::generic_string(default_sysroot));
  fingerprint.add(self_contained ? "self-contained" : "needs-sysroot");
  fingerprint.add(platform::to_string(crt));
  fingerprint.add(platform::to_string(libcpp));
  fingerprint.add(platform::to_string(abi));
  fingerprint.add(capabilities.to_string());
  fingerprint.add(registry_id);
  fingerprint.add(trust_level);
  return fingerprint.finish_hex();
}

std::string ToolchainDescriptor::to_string() const {
  std::string out = label();
  out += " [";
  out += id;
  out += "]";
  out += " host=" + host.to_string();
  if (!supported_targets.empty()) {
    out += " targets=" + str::join(supported_targets, ",");
  }
  return out;
}

std::string ResolvedToolchain::fingerprint_input() const {
  cova::Fingerprint fingerprint;
  fingerprint.add(descriptor.compute_fingerprint());
  fingerprint.add(target.to_string());
  fingerprint.add(abi_signature.fingerprint_key());
  fingerprint.add(sysroot_id);
  fingerprint.add(fs::generic_string(sysroot_path));
  fingerprint.add(sysroot_fingerprint);
  return fingerprint.finish_hex();
}

std::string ResolvedToolchain::to_string() const {
  std::string out = descriptor.label();
  out += " for " + target.to_string();
  if (!sysroot_id.empty()) {
    out += " with sysroot " + sysroot_id;
  }
  return out;
}

}  // namespace cova::toolchain
