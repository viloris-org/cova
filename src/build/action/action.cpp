// Cova — Build Graph action model implementation.
#include "build/action/action.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "build/cache/cache.hpp"
#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"

namespace cova::build {
namespace {

constexpr int kMaxResponseFileDepth = 8;

void add_response_file_identity(Fingerprint& fingerprint, const std::string& argument,
                                const Path& working_dir, int depth);

void add_argument_identity(Fingerprint& fingerprint, const std::string& argument,
                           const Path& working_dir, int depth) {
  fingerprint.add(argument);
  if (argument.size() < 2 || argument.front() != '@') {
    return;
  }
  Path response = Path(argument.substr(1));
  if (response.is_relative()) {
    response = working_dir.empty() ? response : working_dir / response;
  }
  auto contents = fs::read_file(response);
  if (!contents) {
    fingerprint.add("response-file-missing");
    fingerprint.add(fs::generic_string(response));
    return;
  }
  fingerprint.add("response-file");
  // Hash the raw bytes: this captures ordering, quoting, escaping and encoding
  // changes even when a higher-level reader would normalise them.
  fingerprint.add(*contents);
  add_response_file_identity(fingerprint, fs::generic_string(response), working_dir, depth + 1);
}

void add_response_file_identity(Fingerprint& fingerprint, const std::string& path,
                                const Path& working_dir, int depth) {
  if (depth > kMaxResponseFileDepth) {
    fingerprint.add("response-file-depth-limit");
    return;
  }
  auto contents = fs::read_file(Path(path));
  if (!contents) {
    return;
  }
  // Nested response files are part of the real input closure. The compiler
  // expands them, so the identity must follow them too.
  for (const auto& token : str::split_whitespace_quoted(*contents)) {
    if (token.size() > 1 && token.front() == '@') {
      add_argument_identity(fingerprint, token, working_dir, depth);
    }
  }
}

std::string directory_entry_kind(const stdfs::directory_entry& entry) {
  std::error_code ec;
  const auto status = entry.symlink_status(ec);
  if (ec) {
    return "?";
  }
  if (stdfs::is_symlink(status)) {
    return "l";
  }
  if (stdfs::is_directory(status)) {
    return "d";
  }
  if (stdfs::is_regular_file(status)) {
    return "f";
  }
  return "?";
}

// Adding, removing or renaming an entry in an include directory changes which
// header a `#include <name>` resolves to. Hashing the directory listing makes
// that change visible even before the depfile is regenerated.
void add_directory_identity(Fingerprint& fingerprint, std::string_view text,
                            std::set<std::string>& seen) {
  const Path directory(text);
  const std::string key = fs::generic_string(directory);
  if (!seen.insert(key).second) {
    return;
  }
  fingerprint.add(key);
  std::error_code ec;
  if (!stdfs::is_directory(directory, ec)) {
    fingerprint.add("directory-missing");
    return;
  }
  std::vector<std::string> entries;
  for (auto it = stdfs::directory_iterator(directory, ec);
       it != stdfs::directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    const std::string name = it->path().filename().string();
    std::string line = name;
    line.push_back('\t');
    line += directory_entry_kind(*it);
    if (stdfs::is_symlink(it->path(), ec)) {
      std::error_code link_error;
      const Path target = stdfs::read_symlink(it->path(), link_error);
      line += "\t->";
      line += link_error ? std::string("<unreadable>") : fs::generic_string(target);
    }
    entries.push_back(std::move(line));
  }
  std::sort(entries.begin(), entries.end());
  fingerprint.add("directory-listing");
  for (const auto& entry : entries) {
    fingerprint.add(entry);
  }
}


std::vector<std::string> include_directories(const Action& action,
                                             const ActionEnvironment& environment) {
  std::vector<std::string> directories;
  const auto add = [&directories](std::string value) {
    if (!value.empty()) {
      directories.push_back(std::move(value));
    }
  };
  for (std::size_t index = 0; index < action.argv.size(); ++index) {
    const std::string& argument = action.argv[index];
    std::string value;
    for (const std::string_view option : {"-I", "-isystem", "-iquote", "-idirafter",
                                          "-F", "-iframework"}) {
      if (argument == option) {
        if (index + 1 < action.argv.size()) {
          add(action.argv[index + 1]);
        }
        break;
      }
      if (argument.size() > option.size() && argument.substr(0, option.size()) == option &&
          (argument[option.size()] == '=' || option == "-I" || option == "-F")) {
        value = argument.substr(option.size());
        if (!value.empty() && value.front() == '=') {
          value.erase(value.begin());
        }
        add(std::move(value));
        break;
      }
    }
  }
  if (action.kind == ActionKind::Compile) {
    auto add_path_list = [&](const std::string& text) {
      const char separator =
#ifdef _WIN32
          ';';
#else
          ':';
#endif
      for (const auto& part : str::split(text, separator)) {
        add(part);
      }
    };
    for (const auto& [name, value] : environment.environment) {
      if (name == "CPATH" || name == "C_INCLUDE_PATH" || name == "CPLUS_INCLUDE_PATH") {
        add_path_list(value);
      }
    }
  }
  return directories;
}

}  // namespace

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

std::string_view to_string(InputOrigin origin) {
  switch (origin) {
    case InputOrigin::Declared:
      return "declared";
    case InputOrigin::Discovered:
      return "discovered";
    case InputOrigin::Observed:
      return "observed";
  }
  return "declared";
}

std::string Action::cache_key(const ActionEnvironment& env) const {
  cova::Fingerprint fingerprint;
  fingerprint.add(kActionIdentitySchemaVersion);
  fingerprint.add(std::string(to_string(kind)));
  fingerprint.add(std::string(to_string(exec_platform)));
  fingerprint.add(input_closure_complete ? "input-closure=complete" : "input-closure=incomplete");
  fingerprint.add(std::string(to_string(env.isolation)));
  fingerprint.add(env.explicit_environment ? "environment=explicit" : "environment=inherited");

  for (const auto& argument : argv) {
    add_argument_identity(fingerprint, argument, working_dir, 0);
  }
  // The executable itself is an input. A bare command name is resolved by the
  // toolchain identity (which hashes every component); an explicit path is
  // hashed here so replacing that file invalidates the action.
  if (!argv.empty()) {
    const Path program(argv.front());
    if (!program.empty() && (program.has_parent_path() || program.is_absolute())) {
      auto digest = Sha256::hex_of_file(program);
      fingerprint.add("executable");
      fingerprint.add(digest ? *digest : std::string("missing"));
    }
  }
  fingerprint.add(fs::generic_string(working_dir));
  fingerprint.add(env.target.to_string());
  // The effective ABI is the toolchain baseline with the action's flags
  // applied. ABI-neutral flags do not pollute it; unknown flags enter the
  // opaque fingerprint instead of being treated as neutral.
  std::vector<std::string> abi_arguments;
  if (!argv.empty()) {
    abi_arguments.assign(argv.begin() + 1, argv.end());
  }
  fingerprint.add(platform::effective_abi(env.abi, abi_arguments).identity_key());
  fingerprint.add(env.toolchain_fingerprint);
  fingerprint.add(env.sysroot_fingerprint);

  // Environment values participate in the identity. Untracked environments are
  // never reusable, so hashing only their names cannot cause a false hit.
  for (const auto& [name, value] : env.environment) {
    fingerprint.add(name);
    if (env.isolation != proc::EnvironmentIsolation::UntrackedEnvironment) {
      fingerprint.add(value);
    }
  }

  FileHasher hasher;
  computed_input_hashes.clear();
  for (const auto& input : inputs) {
    const std::string path = fs::generic_string(input.path);
    fingerprint.add(path);
    fingerprint.add(input.role);
    fingerprint.add(std::string(to_string(input.origin)));
    std::error_code link_error;
    if (stdfs::is_symlink(input.path, link_error)) {
      fingerprint.add("symlink");
      fingerprint.add(fs::generic_string(stdfs::read_symlink(input.path, link_error)));
    }
    auto digest = hasher.hash_file(input.path);
    if (digest) {
      const std::string hex = cova::to_hex(*digest);
      fingerprint.add(hex);
      computed_input_hashes.emplace_back(path, hex);
    } else {
      fingerprint.add("missing");
      computed_input_hashes.emplace_back(path, "missing");
    }
  }
  // Output paths matter: two actions with identical inputs but different
  // destinations are not interchangeable.
  for (const auto& output : outputs) {
    fingerprint.add(fs::generic_string(output.path));
    fingerprint.add(output.primary ? "primary" : "secondary");
  }

  if (kind == ActionKind::Compile) {
    std::set<std::string> seen;
    for (const auto& directory : include_directories(*this, env)) {
      add_directory_identity(fingerprint, directory, seen);
    }
  }

  std::string key = fingerprint.finish_hex();
  computed_key = key;
  return key;
}

bool Action::inputs_stable(std::string* detail) const {
  FileHasher hasher;
  for (const auto& [path, expected] : computed_input_hashes) {
    auto digest = hasher.hash_file(Path(path));
    const std::string actual = digest ? cova::to_hex(*digest) : std::string("missing");
    if (actual != expected) {
      if (detail != nullptr) {
        *detail = "input changed after discovery: " + path;
      }
      return false;
    }
  }
  return true;
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
