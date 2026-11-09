// Cova — explicit child-process environments (implementation).
#include "core/process/environment.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/hashing/hash.hpp"

namespace cova::proc {
namespace {

#ifdef _WIN32
constexpr bool kCaseInsensitiveNames = true;
#else
constexpr bool kCaseInsensitiveNames = false;
#endif

std::string canonical_name(std::string_view name) {
  std::string out(name);
  if constexpr (kCaseInsensitiveNames) {
    for (char& character : out) {
      character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
  }
  return out;
}

bool same_name(std::string_view left, std::string_view right) {
  return canonical_name(left) == canonical_name(right);
}

// Build-relevant variables that are inherited and audited by default. The list
// is deliberately small: an unknown variable is rejected, not forwarded.
constexpr std::string_view kTrackedNames[] = {
    // Executable lookup and compiler/linker search paths.
    "PATH",
    "CPATH",
    "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH",
    "LIBRARY_PATH",
    "GCC_EXEC_PREFIX",
    "COMPILER_PATH",
    // Reproducible builds and archive timestamps.
    "SOURCE_DATE_EPOCH",
    "ZERO_AR_DATE",
    // Locale: diagnostics and some preprocessing behaviour.
    "LANG",
    "LC_ALL",
    "LC_ADDRESS",
    "LC_COLLATE",
    "LC_CTYPE",
    "LC_IDENTIFICATION",
    "LC_MEASUREMENT",
    "LC_MESSAGES",
    "LC_MONETARY",
    "LC_NAME",
    "LC_NUMERIC",
    "LC_PAPER",
    "LC_TELEPHONE",
    "LC_TIME",
    // Temporary directories.
    "TMPDIR",
    "TEMP",
    "TMP",
    // Dynamic loader search paths (compiler plugins, sanitizer runtimes).
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH",
    // macOS SDK / deployment selection (deployment target is ABI-relevant).
    "SDKROOT",
    "DEVELOPER_DIR",
    "MACOSX_DEPLOYMENT_TARGET",
    "IPHONEOS_DEPLOYMENT_TARGET",
    "TVOS_DEPLOYMENT_TARGET",
    "WATCHOS_DEPLOYMENT_TARGET",
    // Clang configuration files can inject flags; they are build inputs.
    "CLANG_CONFIG_FILE_SYSTEM_DIR",
    "CLANG_CONFIG_FILE_USER_DIR",
    // Windows system variables required by CreateProcessW and the CRT.
    "SystemRoot",
    "SYSTEMROOT",
    "windir",
    "ComSpec",
    "PATHEXT",
    "ProgramFiles",
    "ProgramFiles(x86)",
    "ProgramW6432",
    "ProgramData",
    "USERPROFILE",
    "HOMEDRIVE",
    "HOMEPATH",
    "HOME",
    "NUMBER_OF_PROCESSORS",
    "PROCESSOR_ARCHITECTURE",
    "OS",
    // MSYS2 / MinGW environments select the compiler runtime and DLL paths.
    "MSYSTEM",
    "MINGW_PREFIX",
    "MSYS2_PATH_TYPE",
};

constexpr std::string_view kRejectedNames[] = {
    // Flag bags are build-tool conventions, not compiler inputs. Cova never
    // reads them, and forwarding them would suggest otherwise.
    "CFLAGS",
    "CXXFLAGS",
    "LDFLAGS",
    "CPPFLAGS",
    // Tool overrides would silently replace the resolved toolchain.
    "CC",
    "CXX",
    "LD",
    "AR",
    "AS",
    "RANLIB",
    "NM",
    "STRIP",
    "OBJCOPY",
    "OBJDUMP",
    // Compiler variables that change dependency generation or diagnostics
    // enough that they must not be forwarded implicitly.
    "DEPENDENCIES_OUTPUT",
    "SUNPRO_DEPENDENCIES",
    "GCC_COMPARE_DEBUG",
    "GCC_COLORS",
};

bool in_list(std::string_view name, const auto& list) {
  for (const auto candidate : list) {
    if (same_name(name, candidate)) {
      return true;
    }
  }
  return false;
}

bool is_locale_name(std::string_view name) {
  // Locale variable names are upper-case on every host; matching is
  // case-insensitive so a user-supplied spelling cannot escape the identity.
  const std::string canonical = str::to_lower(std::string(name));
  return canonical.rfind("lc_", 0) == 0;
}

bool is_sensitive_text(std::string_view name) {
  std::string canonical = str::to_lower(std::string(name));
  static constexpr std::string_view markers[] = {
      "secret",   "password", "passwd", "credential", "private_key",
      "api_key",  "apikey",   "access_key", "auth_token", "session_key",
      "session_token", "cookie",
  };
  for (const auto marker : markers) {
    if (canonical.find(marker) != std::string::npos) {
      return true;
    }
  }
  // A TOKEN segment is treated as secret; avoid matching ordinary words such
  // as "TOKENIZERS" by requiring a separated segment.
  std::string segment;
  for (char character : canonical) {
    if (std::isalnum(static_cast<unsigned char>(character))) {
      segment.push_back(character);
    } else {
      if (segment == "token") {
        return true;
      }
      segment.clear();
    }
  }
  return segment == "token";
}

}  // namespace

std::string_view to_string(EnvironmentPolicy policy) {
  switch (policy) {
    case EnvironmentPolicy::Explicit:
      return "explicit";
    case EnvironmentPolicy::InheritedTracked:
      return "inherited-tracked";
    case EnvironmentPolicy::Rejected:
      return "rejected";
    case EnvironmentPolicy::Untracked:
      return "untracked";
  }
  return "rejected";
}

std::string_view to_string(EnvironmentIsolation isolation) {
  switch (isolation) {
    case EnvironmentIsolation::Hermetic:
      return "hermetic";
    case EnvironmentIsolation::TrackedEnvironment:
      return "tracked-environment";
    case EnvironmentIsolation::UntrackedEnvironment:
      return "untracked-environment";
  }
  return "untracked-environment";
}

std::vector<std::pair<std::string, std::string>> EnvironmentSnapshot::variables() const {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(entries.size());
  for (const auto& entry : entries) {
    out.emplace_back(entry.name, entry.value);
  }
  return out;
}

std::map<std::string, std::string, std::less<>> EnvironmentSnapshot::as_map() const {
  std::map<std::string, std::string, std::less<>> out;
  for (const auto& entry : entries) {
    out.emplace(entry.name, entry.value);
  }
  return out;
}

std::string EnvironmentSnapshot::identity() const {
  Fingerprint fingerprint;
  fingerprint.add("cova.environment/1");
  fingerprint.add(to_string(isolation));
  for (const auto& entry : entries) {
    fingerprint.add(entry.name);
    fingerprint.add(to_string(entry.policy));
    if (entry.tracked) {
      fingerprint.add(entry.value);
    } else {
      // An untracked value must never leak into a cache identity (or a shared
      // cache), but the presence and name still distinguish the action.
      fingerprint.add("<untracked>");
    }
  }
  return fingerprint.finish_hex();
}

EnvironmentPolicy environment_policy(std::string_view name) {
  // Rejection wins over the locale prefix: `LC_*` is audited, but a name such
  // as `LC_PASSWORD` is still secret-bearing and must never be forwarded.
  if (in_list(name, kRejectedNames) || is_sensitive_environment_variable(name)) {
    return EnvironmentPolicy::Rejected;
  }
  if (in_list(name, kTrackedNames) || is_locale_name(name)) {
    return EnvironmentPolicy::InheritedTracked;
  }
  return EnvironmentPolicy::Rejected;
}

bool is_tracked_environment_variable(std::string_view name) {
  if (is_rejected_environment_variable(name)) {
    return false;
  }
  return in_list(name, kTrackedNames) || is_locale_name(name);
}

bool is_rejected_environment_variable(std::string_view name) {
  return in_list(name, kRejectedNames) || is_sensitive_environment_variable(name);
}

bool is_sensitive_environment_variable(std::string_view name) {
  return is_sensitive_text(name);
}

Result<EnvironmentSnapshot> collect_environment(const EnvironmentRequest& request) {
  EnvironmentSnapshot snapshot;
  std::vector<EnvironmentEntry> entries;

  auto remove_entry = [&entries](std::string_view name) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const EnvironmentEntry& entry) {
                                   return same_name(entry.name, name);
                                 }),
                  entries.end());
  };

  auto requested_removal = [&request](std::string_view name) {
    return std::any_of(request.removals.begin(), request.removals.end(),
                       [&](const std::string& removed) { return same_name(removed, name); });
  };

  auto reject = [&snapshot](std::string_view name) {
    const std::string text(name);
    if (std::find(snapshot.rejected.begin(), snapshot.rejected.end(), text) ==
        snapshot.rejected.end()) {
      snapshot.rejected.push_back(text);
    }
  };

  for (const auto& [name, value] : process_environment()) {
    if (is_sensitive_environment_variable(name)) {
      reject(name);
      continue;
    }
    if (requested_removal(name)) {
      reject(name);
      continue;
    }
    const EnvironmentPolicy policy = environment_policy(name);
    if (policy == EnvironmentPolicy::InheritedTracked) {
      entries.push_back(EnvironmentEntry{name, value, EnvironmentPolicy::InheritedTracked, true});
    } else if (request.inherit_untracked && !is_rejected_environment_variable(name)) {
      entries.push_back(EnvironmentEntry{name, value, EnvironmentPolicy::Untracked, false});
    } else {
      reject(name);
    }
  }

  for (const auto& variable : request.additions) {
    if (is_sensitive_environment_variable(variable.name)) {
      return fail<EnvironmentSnapshot>(
          Error::make("cova::proc::environment_secret",
                      "sensitive environment variable `" + variable.name +
                          "` cannot be passed to a build action")
              .with("secret-bearing values are never stored in build metadata"));
    }
    if (is_rejected_environment_variable(variable.name)) {
      return fail<EnvironmentSnapshot>(
          Error::make("cova::proc::environment_rejected",
                      "environment variable `" + variable.name + "` is not a build input")
              .with("Cova does not forward flag bags or tool overrides"));
    }
    remove_entry(variable.name);
    if (variable.tracked) {
      entries.push_back(
          EnvironmentEntry{variable.name, variable.value, EnvironmentPolicy::Explicit, true});
    } else {
      entries.push_back(
          EnvironmentEntry{variable.name, variable.value, EnvironmentPolicy::Untracked, false});
    }
  }

  for (const auto& removed : request.removals) {
    remove_entry(removed);
    reject(removed);
  }

  std::sort(entries.begin(), entries.end(), [](const EnvironmentEntry& left,
                                               const EnvironmentEntry& right) {
    const std::string left_name = canonical_name(left.name);
    const std::string right_name = canonical_name(right.name);
    if (left_name != right_name) {
      return left_name < right_name;
    }
    return left.name < right.name;
  });

  bool has_untracked = false;
  bool has_extended_tracked = false;
  for (const auto& entry : entries) {
    if (!entry.tracked) {
      has_untracked = true;
    } else if (entry.policy == EnvironmentPolicy::Explicit &&
               !is_tracked_environment_variable(entry.name)) {
      has_extended_tracked = true;
    }
  }
  if (has_untracked) {
    snapshot.isolation = EnvironmentIsolation::UntrackedEnvironment;
  } else if (has_extended_tracked) {
    snapshot.isolation = EnvironmentIsolation::TrackedEnvironment;
  } else {
    snapshot.isolation = EnvironmentIsolation::Hermetic;
  }
  snapshot.shared_cache_eligible = snapshot.isolation == EnvironmentIsolation::Hermetic;
  snapshot.incremental_eligible = snapshot.isolation != EnvironmentIsolation::UntrackedEnvironment;
  std::sort(snapshot.rejected.begin(), snapshot.rejected.end());
  snapshot.rejected.erase(std::unique(snapshot.rejected.begin(), snapshot.rejected.end()),
                          snapshot.rejected.end());
  snapshot.entries = std::move(entries);
  return snapshot;
}

}  // namespace cova::proc
