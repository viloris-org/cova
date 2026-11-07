// Cova — Build Graph action model implementation.
#include "build/action/action.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "build/cache/cache.hpp"
#include "core/filesystem/filesystem.hpp"

namespace cova::build {

std::string_view to_string(ActionKind kind) {
  switch (kind) {
    case ActionKind::Compile:
      return "compile";
    case ActionKind::Archive:
      return "archive";
    case ActionKind::Link:
      return "link";
    case ActionKind::Run:
      return "run";
    case ActionKind::Copy:
      return "copy";
    case ActionKind::Custom:
      return "custom";
  }
  return "custom";
}

std::string_view to_string(ExecPlatform platform) {
  switch (platform) {
    case ExecPlatform::BuildHost:
      return "build-host";
    case ExecPlatform::TargetDevice:
      return "target-device";
    case ExecPlatform::RemoteHost:
      return "remote-host";
    case ExecPlatform::Unknown:
      break;
  }
  return "unknown";
}

std::string Action::cache_key(const ActionEnvironment& env) const {
  cova::Fingerprint fingerprint;
  fingerprint.add(std::string(to_string(kind)));
  for (const auto& argument : argv) {
    fingerprint.add(argument);
  }
  fingerprint.add(fs::generic_string(working_dir));
  fingerprint.add(env.target.to_string());
  fingerprint.add(env.abi.fingerprint_key());
  fingerprint.add(env.toolchain_fingerprint);
  fingerprint.add(env.sysroot_fingerprint);
  for (const auto& [name, value] : env.environment) {
    fingerprint.add(name);
    fingerprint.add(value);
  }
  FileHasher hasher;
  for (const auto& input : inputs) {
    fingerprint.add(fs::generic_string(input.path));
    fingerprint.add(input.role);
    auto digest = hasher.hash_file(input.path);
    if (digest) {
      fingerprint.add(cova::to_hex(*digest));
    } else {
      fingerprint.add("missing");
    }
  }
  // Output paths matter: two actions with identical inputs but different
  // destinations are not interchangeable.
  for (const auto& output : outputs) {
    fingerprint.add(fs::generic_string(output.path));
  }
  std::string key = fingerprint.finish_hex();
  computed_key = key;
  return key;
}

std::string Action::label_with_output() const {
  std::string out = label;
  if (!outputs.empty()) {
    for (const auto& output : outputs) {
      if (output.primary) {
        out += " -> " + fs::generic_string(output.path);
        break;
      }
    }
  }
  return out;
}

}  // namespace cova::build
