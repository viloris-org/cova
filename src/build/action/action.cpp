// Cova — Build Graph action model implementation.
#include "build/action/action.hpp"

#include <algorithm>
#include <cctype>
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

struct ResponseContext {
  Fingerprint& fingerprint;
  Path working_dir;
  std::set<std::string> active;
  int depth = 0;
  bool identity_complete = true;
};

Path resolve_relative(Path path, const Path& working_dir) {
  if (path.is_relative() && !working_dir.empty()) {
    path = working_dir / path;
  }
  return path;
}

// Files the driver or its subprograms read but that are not expressed as a
// normal argv input: plugins, linker scripts, specs and symbol files. Their
// contents decide the artifact just like an object file does.
void add_auxiliary_file_identity(ResponseContext& context, const Path& file,
                                 std::string_view role) {
  const Path resolved = resolve_relative(file, context.working_dir);
  context.fingerprint.add(role);
  context.fingerprint.add(fs::generic_string(resolved));
  auto digest = Sha256::hex_of_file(resolved);
  if (digest) {
    context.fingerprint.add(*digest);
  } else {
    context.fingerprint.add("missing");
    if (fs::is_regular_file(resolved)) {
      // The file exists but cannot be read: the identity is not proven.
      context.identity_complete = false;
    }
  }
}

// GCC's response-file reader splits on whitespace, strips single/double quotes
// and honours backslash escapes in and outside quotes. Using the generic
// command-line splitter here would mis-resolve a nested `@spaced\ file.rsp`.
std::vector<std::string> split_response_tokens(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  bool has_token = false;
  bool in_single = false;
  bool in_double = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (in_single) {
      if (character == '\'') {
        in_single = false;
      } else {
        current.push_back(character);
        has_token = true;
      }
      continue;
    }
    if (in_double) {
      if (character == '\\' && index + 1 < text.size()) {
        current.push_back(text[++index]);
        has_token = true;
      } else if (character == '"') {
        in_double = false;
      } else {
        current.push_back(character);
        has_token = true;
      }
      continue;
    }
    if (character == '\\' && index + 1 < text.size()) {
      current.push_back(text[++index]);
      has_token = true;
      continue;
    }
    if (character == '\'') {
      in_single = true;
      has_token = true;
      continue;
    }
    if (character == '"') {
      in_double = true;
      has_token = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      if (has_token) {
        tokens.push_back(current);
        current.clear();
        has_token = false;
      }
      continue;
    }
    current.push_back(character);
    has_token = true;
  }
  if (has_token) {
    tokens.push_back(current);
  }
  return tokens;
}

void add_response_file_identity(ResponseContext& context, const Path& file) {
  if (context.depth > kMaxResponseFileDepth) {
    context.fingerprint.add("response-file-depth-limit");
    context.fingerprint.add(fs::generic_string(file));
    context.identity_complete = false;
    return;
  }
  const Path resolved = resolve_relative(file, context.working_dir);
  const std::string key = fs::generic_string(fs::canonical_for_compare(resolved));
  if (context.active.find(key) != context.active.end()) {
    // GCC and Clang both reject recursive @-file expansion. Record the cycle
    // deterministically and refuse reuse rather than recursing forever.
    context.fingerprint.add("response-file-cycle");
    context.fingerprint.add(key);
    context.identity_complete = false;
    return;
  }
  auto contents = fs::read_file(resolved);
  context.fingerprint.add("response-file");
  context.fingerprint.add(key);
  if (!contents) {
    context.fingerprint.add("response-file-missing");
    return;
  }
  // Hash the raw bytes: this captures ordering, quoting, escaping and encoding
  // changes even when a higher-level reader would normalise them.
  context.fingerprint.add(*contents);
  context.active.insert(key);
  ++context.depth;
  for (const auto& token : split_response_tokens(*contents)) {
    if (token.size() > 1 && token.front() == '@') {
      add_response_file_identity(context, Path(token.substr(1)));
    }
  }
  --context.depth;
  context.active.erase(key);
}

void add_comma_separated_identity(ResponseContext& context, std::string_view rest,
                                  std::string_view role) {
  const auto parts = str::split(rest, ',');
  for (std::size_t index = 0; index < parts.size(); ++index) {
    const std::string& part = parts[index];
    if (part.size() > 1 && part.front() == '@') {
      add_response_file_identity(context, Path(part.substr(1)));
      continue;
    }
    for (const std::string_view prefix : {"--script=", "--version-script=",
                                          "--dynamic-list=", "--retain-symbols-file=",
                                          "--just-symbols="}) {
      if (part.size() > prefix.size() && part.substr(0, prefix.size()) == prefix) {
        add_auxiliary_file_identity(context, Path(part.substr(prefix.size())), role);
      }
    }
    if ((part == "-T" || part == "-R" || part == "--script" || part == "--version-script" ||
         part == "--dynamic-list" || part == "--retain-symbols-file" ||
         part == "--just-symbols" || part == "-plugin") &&
        index + 1 < parts.size()) {
      add_auxiliary_file_identity(context, Path(parts[++index]), role);
    }
    if (part.rfind("-plugin=", 0) == 0 && part.size() > 8) {
      add_auxiliary_file_identity(context, Path(part.substr(8)), role);
    }
  }
}

void add_argument_identity(ResponseContext& context, const std::string& argument) {
  context.fingerprint.add(argument);
  if (argument.size() > 1 && argument.front() == '@') {
    add_response_file_identity(context, Path(argument.substr(1)));
    return;
  }
  for (const std::string_view prefix : {"-Wl,", "-Wa,", "-Wp,"}) {
    if (argument.size() > prefix.size() && argument.substr(0, prefix.size()) == prefix) {
      add_comma_separated_identity(context, std::string_view(argument).substr(prefix.size()),
                                   prefix == "-Wl," ? "linker-input" : "tool-input");
      return;
    }
  }
  for (const std::string_view prefix : {"-fplugin=", "-specs=", "--specs=", "--script=",
                                        "--version-script=", "--dynamic-list=",
                                        "--retain-symbols-file=", "--just-symbols="}) {
    if (argument.size() > prefix.size() && argument.substr(0, prefix.size()) == prefix) {
      add_auxiliary_file_identity(context, Path(argument.substr(prefix.size())), "driver-input");
      return;
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
bool add_directory_identity(Fingerprint& fingerprint, std::string_view text,
                            std::set<std::string>& seen) {
  const Path directory(text);
  const std::string key = fs::generic_string(directory);
  if (!seen.insert(key).second) {
    return true;
  }
  fingerprint.add(key);
  std::error_code ec;
  const bool is_directory = stdfs::is_directory(directory, ec);
  if (ec) {
    // The directory status could not be established (for example a permission
    // error on a parent). This is not equivalent to a missing directory.
    fingerprint.add("directory-check-incomplete");
    return false;
  }
  if (!is_directory) {
    // A missing include directory is complete evidence: its listing is empty.
    // If the directory appears later, the key changes.
    fingerprint.add("directory-missing");
    return true;
  }
  std::vector<std::string> entries;
  bool complete = true;
  stdfs::directory_iterator it(directory, ec);
  if (ec) {
    complete = false;
  }
  const stdfs::directory_iterator end;
  while (!ec && it != end) {
    const std::string name = it->path().filename().string();
    const std::string kind = directory_entry_kind(*it);
    if (kind == "?") {
      // An entry whose type cannot be established makes the listing partial.
      complete = false;
    }
    std::string line = name;
    line.push_back('\t');
    line += kind;
    std::error_code link_error;
    if (stdfs::is_symlink(it->path(), link_error)) {
      if (link_error) {
        complete = false;
      }
      std::error_code target_error;
      const Path target = stdfs::read_symlink(it->path(), target_error);
      line += "\t->";
      line += target_error ? std::string("<unreadable>") : fs::generic_string(target);
      if (target_error) {
        complete = false;
      }
    }
    entries.push_back(std::move(line));
    it.increment(ec);
  }
  if (ec) {
    // A partial listing is not a proof of the input closure. Keep the entries
    // for diagnostics, but mark the identity incomplete so nothing is reused.
    complete = false;
    fingerprint.add("directory-enumeration-incomplete");
  }
  std::sort(entries.begin(), entries.end());
  fingerprint.add("directory-listing");
  for (const auto& entry : entries) {
    fingerprint.add(entry);
  }
  return complete;
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
      if (argument.size() > option.size() && argument.substr(0, option.size()) == option) {
        // Attached forms: `-I/path`, `-isystem/path`, `-isystem=path`.
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
      if (str::iequals(name, "CPATH") || str::iequals(name, "C_INCLUDE_PATH") ||
          str::iequals(name, "CPLUS_INCLUDE_PATH")) {
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

  input_identity_complete = true;
  ResponseContext response_context{fingerprint, working_dir, {}, 0, true};
  for (std::size_t index = 0; index < argv.size(); ++index) {
    add_argument_identity(response_context, argv[index]);
    if ((argv[index] == "-T" || argv[index] == "-specs" || argv[index] == "--specs") &&
        index + 1 < argv.size()) {
      add_auxiliary_file_identity(response_context, Path(argv[index + 1]), "driver-input");
    }
    if (argv[index] == "-Xclang" && index + 1 < argv.size() && argv[index + 1] == "-load") {
      // Clang accepts both `-Xclang -load plugin.so` and the common
      // `-Xclang -load -Xclang plugin.so` form.
      if (index + 2 < argv.size()) {
        if (argv[index + 2] == "-Xclang" && index + 3 < argv.size()) {
          add_auxiliary_file_identity(response_context, Path(argv[index + 3]), "plugin");
        } else {
          add_auxiliary_file_identity(response_context, Path(argv[index + 2]), "plugin");
        }
      }
    }
  }
  if (!response_context.identity_complete) {
    input_identity_complete = false;
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
      if (!add_directory_identity(fingerprint, directory, seen)) {
        input_identity_complete = false;
      }
    }
  }
  fingerprint.add(input_identity_complete ? "identity=complete" : "identity=incomplete");

  std::string key = fingerprint.finish_hex();
  computed_key = key;
  return key;
}

bool Action::inputs_stable(std::string* detail) const {
  if (!input_identity_complete) {
    if (detail != nullptr) {
      *detail = "the input identity is incomplete";
    }
    return false;
  }
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
