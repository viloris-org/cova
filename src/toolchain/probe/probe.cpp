// Cova — toolchain capability probing (PRD §7.6).
#include "toolchain/probe/probe.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/filesystem/filesystem.hpp"
#include "core/hashing/hash.hpp"
#include "core/process/process.hpp"
#include "core/toml/toml.hpp"

namespace cova::toolchain {
namespace {

std::vector<std::string> driver_flags(const ToolchainDescriptor& descriptor,
                                      const ProbeOptions& options) {
  std::vector<std::string> flags;
  if ((descriptor.family == "clang" || descriptor.family == "clang-cl") &&
      options.target.is_valid() && options.target != platform::host_triple()) {
    flags.push_back("--target=" + options.target.to_string());
  }
  if (!options.sysroot.empty() && fs::is_directory(options.sysroot)) {
    flags.push_back("--sysroot=" + fs::generic_string(options.sysroot));
  }
  return flags;
}

void apply_probe_environment(const ProbeOptions& options, proc::ProcessSpec& spec) {
  if (!options.environment.empty()) {
    spec.environment = options.environment;
  }
}

std::string compiler_program(const ToolchainDescriptor& descriptor) {
  const auto cxx = descriptor.component_path("cxx");
  if (!cxx.empty()) {
    return fs::generic_string(cxx);
  }
  return fs::generic_string(descriptor.component_path("cc"));
}

Result<fs::Path> make_scratch(const ProbeOptions& options, std::string_view tag) {
  fs::Path parent = options.work_dir;
  if (parent.empty()) {
    parent = stdfs::temp_directory_path();
  }
  if (!fs::is_directory(parent)) {
    auto status = fs::create_directories(parent);
    if (!status) {
      return fail<fs::Path>(status.error());
    }
  }
  auto dir = fs::make_temp_dir(parent, "cova-probe-" + std::string(tag) + "-");
  if (!dir) {
    return fail<fs::Path>(dir.error());
  }
  return *dir;
}

Status run_probe_command(const ToolchainDescriptor& descriptor, const ProbeOptions& options,
                         const std::vector<std::string>& extra_args, std::string_view tag) {
  auto scratch = make_scratch(options, tag);
  if (!scratch) {
    return fail(scratch.error());
  }
  const fs::Path source = *scratch / "main.cpp";
  const fs::Path output = *scratch / "main.out";
  std::string source_text = "int main() { return 0; }\n";
  if (tag == "c") {
    source_text = "int main(void) { return 0; }\n";
  }
  auto write = fs::write_file(source, source_text);
  if (!write) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return write;
  }

  std::vector<std::string> args = driver_flags(descriptor, options);
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  args.push_back(fs::generic_string(source));
  args.push_back("-o");
  args.push_back(fs::generic_string(output));

  proc::ProcessSpec spec;
  spec.program = compiler_program(descriptor);
  spec.args = std::move(args);
  spec.cwd = *scratch;
  spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, spec);
  auto result = proc::run(spec);
  if (!result) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return fail(result.error());
  }
  const bool success = result->success() && fs::is_regular_file(output);
  if (!options.keep_artifacts) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
  }
  if (!success) {
    Error error = Error::make("cova::toolchain::probe_failed",
                              "the `" + std::string(tag) + "` probe failed");
    const std::string diagnostics =
        str::trim(result->stderr_text.empty() ? result->stdout_text : result->stderr_text);
    if (!diagnostics.empty()) {
      error.with(diagnostics);
    }
    if (result->timed_out) {
      error.with("the compiler timed out after " + std::to_string(options.timeout_ms) + "ms");
    }
    return fail(std::move(error));
  }
  return ok();
}

// Probe cache identity. Every field that can change an observation is part of
// the key, so an implementation or rule change cannot reuse an older report.
constexpr std::string_view kProbeSchemaVersion = "cova.probe/2";
constexpr std::string_view kProbeLogicVersion = "cova.probe.logic/2";
constexpr std::string_view kProbeImplementationVersion = "cova-probe/2";
// Bump this manifest whenever probe rules or their inputs change. It is hashed
// into the key so the requirement is enforced by code, not by a comment.
constexpr std::string_view kProbeSourceManifest =
    "predefined-macros-v1|cpp-codegen-v1|preprocess-v1|archive-v1|link-v1|"
    "stdlib-headers-v1|cpp26-baseline-v1|secondary-flags-v1";

std::string probe_source_hash() {
  return to_hex(Sha256::of(kProbeSourceManifest));
}

std::string probe_environment_identity(const ProbeOptions& options) {
  if (!options.environment_identity.empty()) {
    return options.environment_identity;
  }
  if (!options.environment.empty()) {
    Fingerprint fingerprint;
    fingerprint.add("cova.probe.environment/1");
    auto variables = options.environment;
    std::sort(variables.begin(), variables.end());
    for (const auto& [name, value] : variables) {
      fingerprint.add(name);
      fingerprint.add(value);
    }
    return fingerprint.finish_hex();
  }
  auto snapshot = proc::collect_environment();
  return snapshot ? snapshot->identity() : std::string("uncontrolled");
}

std::string probe_key(const ToolchainDescriptor& descriptor, const ProbeOptions& options,
                      std::string_view variant) {
  Fingerprint fingerprint;
  fingerprint.add(kProbeSchemaVersion);
  fingerprint.add(kProbeLogicVersion);
  fingerprint.add(probe_source_hash());
  fingerprint.add(kProbeImplementationVersion);
  fingerprint.add(descriptor.content_hash.empty() ? descriptor.id : descriptor.content_hash);
  fingerprint.add(descriptor.family);
  fingerprint.add(descriptor.version.to_string());
  fingerprint.add(fs::generic_string(descriptor.resource_dir));
  fingerprint.add(fs::generic_string(descriptor.default_sysroot));
  fingerprint.add(options.target.to_string());
  fingerprint.add(fs::generic_string(options.sysroot));
  fingerprint.add(probe_environment_identity(options));
  fingerprint.add(static_cast<std::int64_t>(options.timeout_ms));
  fingerprint.add(variant);
  auto required = options.required;
  std::sort(required.begin(), required.end());
  for (const auto& name : required) {
    fingerprint.add(name);
  }
  return fingerprint.finish_hex();
}

}  // namespace

Status probe_c_compilation(const ToolchainDescriptor& descriptor, const ProbeOptions& options) {
  return run_probe_command(descriptor, options, {"-x", "c", "-std=c17", "-c"}, "c");
}

Status probe_cpp_compilation(const ToolchainDescriptor& descriptor,
                             const ProbeOptions& options) {
  return run_probe_command(descriptor, options, {"-x", "c++", "-std=c++26", "-c"}, "c++");
}

Status probe_static_library(const ToolchainDescriptor& descriptor, const ProbeOptions& options) {
  auto scratch = make_scratch(options, "archive");
  if (!scratch) {
    return fail(scratch.error());
  }
  const auto source = *scratch / "lib.cpp";
  const auto object = *scratch / "lib.o";
  const auto archive = *scratch / "libcova_probe.a";
  auto write = fs::write_file(source, "int cova_probe() { return 0; }\n");
  if (!write) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return write;
  }

  std::vector<std::string> compile_args = driver_flags(descriptor, options);
  compile_args.insert(compile_args.end(), {"-x", "c++", "-std=c++26", "-c",
                                           fs::generic_string(source), "-o",
                                           fs::generic_string(object)});
  proc::ProcessSpec compile_spec;
  compile_spec.program = compiler_program(descriptor);
  compile_spec.args = std::move(compile_args);
  compile_spec.cwd = *scratch;
  compile_spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, compile_spec);
  auto compiled = proc::run(compile_spec);
  if (!compiled || !compiled->success()) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return fail(Error::make("cova::toolchain::probe_failed",
                            "the archive probe could not compile its input"));
  }

  const auto archiver = descriptor.component_path("archiver");
  if (archiver.empty()) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return fail(Error::make("cova::toolchain::probe_failed",
                            "the toolchain has no archiver component"));
  }
  proc::ProcessSpec archive_spec;
  archive_spec.program = archiver;
  archive_spec.args = {"rcs", fs::generic_string(archive), fs::generic_string(object)};
  archive_spec.cwd = *scratch;
  archive_spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, archive_spec);
  auto archived = proc::run(archive_spec);
  const bool success = archived && archived->success() && fs::is_regular_file(archive);
  auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
  (void)cleanup;
  if (!success) {
    return fail(Error::make("cova::toolchain::probe_failed",
                            "the toolchain could not create a static library"));
  }
  return ok();
}

Status probe_executable_link(const ToolchainDescriptor& descriptor, const ProbeOptions& options) {
  return run_probe_command(descriptor, options, {"-x", "c++", "-std=c++26"}, "link");
}

Status probe_stdlib_headers(const ToolchainDescriptor& descriptor,
                            const ProbeOptions& options) {
  auto scratch = make_scratch(options, "stdlib");
  if (!scratch) {
    return fail(scratch.error());
  }
  const auto source = *scratch / "headers.cpp";
  const auto object = *scratch / "headers.o";
  auto write = fs::write_file(source,
                              "#include <vector>\n#include <string>\n#include <iostream>\n"
                              "#include <map>\n#include <memory>\n"
                              "int main() { std::vector<int> v; return v.size(); }\n");
  if (!write) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return write;
  }
  std::vector<std::string> args = driver_flags(descriptor, options);
  args.insert(args.end(), {"-x", "c++", "-std=c++26", "-c", fs::generic_string(source), "-o",
                           fs::generic_string(object)});
  proc::ProcessSpec spec;
  spec.program = compiler_program(descriptor);
  spec.args = std::move(args);
  spec.cwd = *scratch;
  spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, spec);
  auto result = proc::run(spec);
  const bool success = result && result->success() && fs::is_regular_file(object);
  auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
  (void)cleanup;
  if (!success) {
    return fail(Error::make("cova::toolchain::probe_failed",
                            "the standard library headers are not usable for C++26"));
  }
  return ok();
}

Status probe_cpp26_baseline(const ToolchainDescriptor& descriptor,
                            const ProbeOptions& options) {
  constexpr std::string_view source_text = R"(
#include <expected>
#include <print>
#include <ranges>
#include <concepts>
#include <vector>

template <class T>
concept Addable = requires(T a, T b) { a + b; };

struct Point { int x = 0; int y = 0; };

int main() {
  auto value = std::expected<int, int>{42};
  Point point{.x = 1, .y = 2};
  (void)point;
  std::vector<int> numbers{1, 2, 3};
  auto doubled = numbers | std::views::transform([](int n) { return n * 2; });
  (void)doubled;
  std::println("{}", *value);
  return 0;
}
)";
  auto scratch = make_scratch(options, "cxx26");
  if (!scratch) {
    return fail(scratch.error());
  }
  const auto source = *scratch / "baseline.cpp";
  auto write = fs::write_file(source, source_text);
  if (!write) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return write;
  }
  const auto output = *scratch / "baseline.out";
  std::vector<std::string> args = driver_flags(descriptor, options);
  args.insert(args.end(), {"-std=c++26", "-x", "c++", fs::generic_string(source), "-o",
                           fs::generic_string(output)});
  proc::ProcessSpec spec;
  spec.program = compiler_program(descriptor);
  spec.args = std::move(args);
  spec.cwd = *scratch;
  spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, spec);
  auto result = proc::run(spec);
  const bool success = result && result->success() && fs::is_regular_file(output);
  auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
  (void)cleanup;
  if (!success) {
    Error error = Error::make("cova::toolchain::probe_failed",
                              "the compiler does not accept the C++26 baseline");
    if (result && !result->stderr_text.empty()) {
      error.with(str::trim(result->stderr_text));
    }
    return fail(std::move(error));
  }
  return ok();
}

Result<std::map<std::string, std::string, std::less<>>> probe_abi_facts(
    const ToolchainDescriptor& descriptor, const ProbeOptions& options) {
  auto scratch = make_scratch(options, "abi");
  if (!scratch) {
    return fail<std::map<std::string, std::string, std::less<>>>(scratch.error());
  }
  const auto source = *scratch / "abi.cpp";
  // Include real headers so the standard library's ABI configuration macros
  // (for example _GLIBCXX_USE_CXX11_ABI) are visible, not only compiler
  // builtins.
  auto write = fs::write_file(source,
                              "#include <cstddef>\n#include <string>\n#include <vector>\n");
  if (!write) {
    auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
    (void)cleanup;
    return fail<std::map<std::string, std::string, std::less<>>>(write.error());
  }
  std::vector<std::string> args = driver_flags(descriptor, options);
  args.insert(args.end(), {"-x", "c++", "-std=c++26", "-dM", "-E",
                           fs::generic_string(source)});
  proc::ProcessSpec spec;
  spec.program = compiler_program(descriptor);
  spec.args = std::move(args);
  spec.cwd = *scratch;
  spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, spec);
  auto result = proc::run(spec);
  auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
  (void)cleanup;
  if (!result) {
    return fail<std::map<std::string, std::string, std::less<>>>(result.error());
  }
  if (!result->success()) {
    return fail<std::map<std::string, std::string, std::less<>>>(
        Error::make("cova::toolchain::probe_failed",
                    "the compiler could not dump its predefined ABI macros"));
  }
  std::map<std::string, std::string, std::less<>> facts;
  for (const auto& line : str::split(result->stdout_text, '\n')) {
    if (!str::starts_with(line, "#define ")) {
      continue;
    }
    const std::string_view rest(line.data() + 8, line.size() - 8);
    const std::size_t name_end = rest.find_first_of(" \t");
    if (name_end == std::string_view::npos) {
      facts[std::string(rest)] = "1";
    } else {
      facts[std::string(rest.substr(0, name_end))] =
          str::trim(rest.substr(name_end + 1));
    }
  }
  return facts;
}

namespace {

struct ProbeStep {
  const char* name;
  Status (*function)(const ToolchainDescriptor&, const ProbeOptions&);
};

}  // namespace

Result<ProbeReport> probe(const ToolchainDescriptor& descriptor, const ProbeOptions& options) {
  ProbeReport report;
  report.capabilities = descriptor.capabilities;
  report.capabilities.c_compilation = false;
  report.capabilities.cpp_compilation = false;
  report.capabilities.preprocess = false;
  report.capabilities.static_library = false;
  report.capabilities.executable_link = false;
  report.capabilities.stdlib_headers = false;
  report.capabilities.cpp26_language = false;
  report.capabilities.cpp26_library = false;
  report.capabilities.shared_library = false;

  const bool full = options.required.empty();
  const auto wanted = [&](std::string_view name) {
    if (full) {
      return true;
    }
    return std::find(options.required.begin(), options.required.end(), std::string(name)) !=
           options.required.end();
  };

  auto record = [&](std::string name, const Status& status, bool& capability,
                    bool fatal) -> void {
    if (status) {
      capability = true;
      report.notes.push_back(name + ": ok");
    } else {
      capability = false;
      report.failures.push_back(name + ": " + status.error().message);
      if (fatal) {
        report.succeeded = false;
      }
    }
  };

  if (wanted("c")) {
    auto status = probe_c_compilation(descriptor, options);
    record("C compilation", status, report.capabilities.c_compilation, false);
  }
  if (wanted("cpp")) {
    auto status = probe_cpp_compilation(descriptor, options);
    record("C++ compilation", status, report.capabilities.cpp_compilation, true);
  }
  {
    // ABI facts are evidence, not a gate: a failed dump leaves every fact
    // Unknown instead of inventing defaults, and the toolchain can still be
    // used for non-ABI work.
    auto facts = probe_abi_facts(descriptor, options);
    if (facts) {
      report.abi_facts = std::move(*facts);
      report.notes.push_back("ABI facts: " +
                             std::to_string(report.abi_facts.size()) + " macro(s)");
    } else {
      report.failures.push_back("ABI facts: " + facts.error().message);
    }
  }
  // Preprocessing is implied by a successful compile, but a separate invocation
  // makes the capability honest if the driver has an unusual configuration.
  if (wanted("preprocess")) {
    auto scratch = make_scratch(options, "preprocess");
    if (scratch) {
      const auto source = *scratch / "pre.cpp";
      auto write = fs::write_file(source, "#define COVA_PROBE 1\nint x = COVA_PROBE;\n");
      if (write) {
        std::vector<std::string> args = driver_flags(descriptor, options);
        args.insert(args.end(), {"-E", "-x", "c++", fs::generic_string(source)});
        proc::ProcessSpec spec;
        spec.program = compiler_program(descriptor);
        spec.args = std::move(args);
        spec.cwd = *scratch;
        spec.timeout = std::chrono::milliseconds(options.timeout_ms);
  apply_probe_environment(options, spec);
        auto result = proc::run(spec);
        const bool success = result && result->success() &&
                             result->stdout_text.find("int x = 1;") != std::string::npos;
        record("preprocessing", success ? Status{} :
                                               fail(Error::make("cova::toolchain::probe_failed",
                                                                "preprocessing failed")),
               report.capabilities.preprocess, false);
      }
      auto cleanup = fs::remove_tree(*scratch, scratch->parent_path());
      (void)cleanup;
    }
  }
  if (wanted("archive")) {
    auto status = probe_static_library(descriptor, options);
    record("static library", status, report.capabilities.static_library, false);
  }
  if (wanted("link")) {
    auto status = probe_executable_link(descriptor, options);
    record("executable link", status, report.capabilities.executable_link, true);
  }
  if (wanted("stdlib")) {
    auto status = probe_stdlib_headers(descriptor, options);
    record("standard library headers", status, report.capabilities.stdlib_headers, false);
  }
  if (wanted("c++26")) {
    auto status = probe_cpp26_baseline(descriptor, options);
    record("C++26 baseline", status, report.capabilities.cpp26_language, false);
    report.capabilities.cpp26_library = report.capabilities.cpp26_language;
  }

  // Secondary capabilities are probed with the same compiler but do not make
  // the toolchain unusable when they are missing.
  auto cheap_probe = [&](std::string_view tag, std::vector<std::string> args) -> bool {
    auto status = run_probe_command(descriptor, options, args, tag);
    return status.has_value();
  };
  if (wanted("shared")) {
    report.capabilities.shared_library = cheap_probe(
        "shared", {"-x", "c++", "-std=c++26", "-shared", "-fPIC", "-c"});
  }
  if (wanted("lto")) {
    report.capabilities.lto = cheap_probe("lto", {"-x", "c++", "-std=c++26", "-flto", "-c"});
  }
  if (wanted("sanitizers")) {
    report.capabilities.sanitizers = cheap_probe(
        "sanitizer", {"-x", "c++", "-std=c++26", "-fsanitize=address", "-c"});
  }
  if (wanted("coverage")) {
    report.capabilities.coverage = cheap_probe(
        "coverage", {"-x", "c++", "-std=c++26", "--coverage", "-c"});
  }
  if (wanted("threads")) {
    report.capabilities.threads_available = cheap_probe(
        "threads", {"-x", "c++", "-std=c++26", "-pthread", "-c"});
  }
  report.capabilities.cpp_modules_scan = false;
  report.capabilities.modules = false;
  return report;
}

Result<ProbeReport> load_cached_probe(const stdfs::path& cache_dir,
                                      const ToolchainDescriptor& descriptor,
                                      const ProbeOptions& options, std::string_view variant) {
  const std::string key = probe_key(descriptor, options, variant);
  const auto path = cache_dir / "probes" / (key + ".toml");
  if (!fs::is_regular_file(path)) {
    return fail<ProbeReport>(Error::make("cova::toolchain::probe_cache_miss",
                                         "no cached probe for this toolchain"));
  }
  auto document = toml::parse_file(fs::generic_string(path));
  if (!document || !document->is_table()) {
    return fail<ProbeReport>(Error::make("cova::toolchain::probe_cache_corrupt",
                                         "the cached probe report is not valid TOML"));
  }
  const auto& table = *document->as_table();
  auto text = [&](std::string_view field) -> std::string {
    auto it = table.find(field);
    return it != table.end() && it->second.is_string() ? it->second.as_string() : std::string();
  };
  // A version or identity mismatch is a miss, never a silent compatibility
  // decision: an old report is evidence about a different probe definition.
  if (text("schema-version") != kProbeSchemaVersion ||
      text("logic-version") != kProbeLogicVersion ||
      text("source-hash") != probe_source_hash() ||
      text("probe-identity") != key ||
      text("toolchain-identity") !=
          (descriptor.content_hash.empty() ? descriptor.id : descriptor.content_hash) ||
      text("environment-identity") != probe_environment_identity(options)) {
    return fail<ProbeReport>(Error::make("cova::toolchain::probe_cache_miss",
                                         "the cached probe report has an incompatible version"));
  }
  auto boolean = [&](std::string_view field) -> bool {
    auto it = table.find(field);
    return it != table.end() && it->second.is_bool() && it->second.as_bool();
  };
  auto strings = [&](std::string_view field) -> std::vector<std::string> {
    auto it = table.find(field);
    return it == table.end() ? std::vector<std::string>{} : it->second.as_string_array();
  };
  ProbeReport report;
  report.capabilities.c_compilation = boolean("c");
  report.capabilities.cpp_compilation = boolean("cpp");
  report.capabilities.preprocess = boolean("preprocess");
  report.capabilities.static_library = boolean("archive");
  report.capabilities.shared_library = boolean("shared");
  report.capabilities.executable_link = boolean("link");
  report.capabilities.stdlib_headers = boolean("stdlib");
  report.capabilities.cpp26_language = boolean("cpp26_language");
  report.capabilities.cpp26_library = boolean("cpp26_library");
  report.capabilities.lto = boolean("lto");
  report.capabilities.modules = boolean("modules");
  report.capabilities.sanitizers = boolean("sanitizers");
  report.capabilities.coverage = boolean("coverage");
  report.capabilities.debug_info = boolean("debug_info");
  report.capabilities.threads_available = boolean("threads");
  report.capabilities.cpp_modules_scan = boolean("module_scan");
  report.succeeded = boolean("succeeded");
  report.notes = strings("notes");
  report.failures = strings("failures");
  auto facts_it = table.find("abi-fact");
  if (facts_it != table.end() && facts_it->second.is_table()) {
    for (const auto& [name, value] : *facts_it->second.as_table()) {
      if (value.is_string()) {
        report.abi_facts.emplace(name, value.as_string());
      }
    }
  }
  return report;
}

namespace {

toml::Value bool_value(bool value) { return toml::Value(value); }

toml::Value strings_value(const std::vector<std::string>& values) {
  toml::Array array;
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return toml::Value(std::move(array));
}

}  // namespace

Status store_cached_probe(const stdfs::path& cache_dir, const ToolchainDescriptor& descriptor,
                          const ProbeOptions& options, const ProbeReport& report,
                          std::string_view variant) {
  const auto directory = cache_dir / "probes";
  auto created = fs::create_directories(directory);
  if (!created) {
    return created;
  }
  const std::string key = probe_key(descriptor, options, variant);
  toml::Table table;
  table.emplace("schema-version", toml::Value(std::string(kProbeSchemaVersion)));
  table.emplace("logic-version", toml::Value(std::string(kProbeLogicVersion)));
  table.emplace("source-hash", toml::Value(probe_source_hash()));
  table.emplace("implementation-version", toml::Value(std::string(kProbeImplementationVersion)));
  table.emplace("probe-identity", toml::Value(key));
  table.emplace("toolchain-identity",
                toml::Value(descriptor.content_hash.empty() ? descriptor.id
                                                            : descriptor.content_hash));
  table.emplace("target", toml::Value(options.target.to_string()));
  table.emplace("sysroot", toml::Value(fs::generic_string(options.sysroot)));
  table.emplace("environment-identity", toml::Value(probe_environment_identity(options)));
  table.emplace("id", toml::Value(descriptor.id));
  table.emplace("content_hash", toml::Value(descriptor.content_hash));
  const auto& c = report.capabilities;
  table.emplace("c", bool_value(c.c_compilation));
  table.emplace("cpp", bool_value(c.cpp_compilation));
  table.emplace("preprocess", bool_value(c.preprocess));
  table.emplace("archive", bool_value(c.static_library));
  table.emplace("shared", bool_value(c.shared_library));
  table.emplace("link", bool_value(c.executable_link));
  table.emplace("stdlib", bool_value(c.stdlib_headers));
  table.emplace("cpp26_language", bool_value(c.cpp26_language));
  table.emplace("cpp26_library", bool_value(c.cpp26_library));
  table.emplace("lto", bool_value(c.lto));
  table.emplace("modules", bool_value(c.modules));
  table.emplace("sanitizers", bool_value(c.sanitizers));
  table.emplace("coverage", bool_value(c.coverage));
  table.emplace("debug_info", bool_value(c.debug_info));
  table.emplace("threads", bool_value(c.threads_available));
  table.emplace("module_scan", bool_value(c.cpp_modules_scan));
  table.emplace("succeeded", bool_value(report.succeeded));
  table.emplace("message", toml::Value(std::string(report.succeeded ? "ok" : "failed")));
  table.emplace("notes", strings_value(report.notes));
  table.emplace("failures", strings_value(report.failures));
  toml::Table facts;
  for (const auto& [name, value] : report.abi_facts) {
    facts.emplace(name, toml::Value(value));
  }
  table.emplace("abi-fact", toml::Value(std::move(facts)));
  return fs::write_file_atomic(directory / (key + ".toml"), toml::write(table));
}

Result<ProbeReport> load_cached_probe(const stdfs::path& cache_dir,
                                      const ToolchainDescriptor& descriptor,
                                      std::string_view variant) {
  return load_cached_probe(cache_dir, descriptor, ProbeOptions{}, variant);
}

Status store_cached_probe(const stdfs::path& cache_dir, const ToolchainDescriptor& descriptor,
                          const ProbeReport& report, std::string_view variant) {
  return store_cached_probe(cache_dir, descriptor, ProbeOptions{}, report, variant);
}

std::string format_capability_table(const ToolchainCapabilities& capabilities) {
  struct Row {
    std::string_view name;
    bool value;
  };
  const Row rows[] = {
      {"C compilation", capabilities.c_compilation},
      {"C++ compilation", capabilities.cpp_compilation},
      {"preprocessing", capabilities.preprocess},
      {"static library", capabilities.static_library},
      {"shared library", capabilities.shared_library},
      {"executable link", capabilities.executable_link},
      {"C++26 language", capabilities.cpp26_language},
      {"C++26 library", capabilities.cpp26_library},
      {"standard library headers", capabilities.stdlib_headers},
      {"LTO", capabilities.lto},
      {"C++ modules", capabilities.modules},
      {"sanitizers", capabilities.sanitizers},
      {"coverage", capabilities.coverage},
      {"debug info", capabilities.debug_info},
      {"threads", capabilities.threads_available},
      {"module scan", capabilities.cpp_modules_scan},
  };
  std::string out;
  for (const auto& row : rows) {
    out += "  ";
    out += row.name;
    out += ": ";
    out += row.value ? "yes" : "no";
    out += "\n";
  }
  return out;
}

}  // namespace cova::toolchain
