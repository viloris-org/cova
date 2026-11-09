// Cova — ABI model and compatibility rules (PRD §6.3, §8.5, §10.5).
#include "platform/abi/abi.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/hashing/hash.hpp"
namespace cova::platform {

std::string_view to_string(AbiFactState state) {
  switch (state) {
    case AbiFactState::Known:
      return "known";
    case AbiFactState::Unknown:
      return "unknown";
    case AbiFactState::Unsupported:
      return "unsupported";
    case AbiFactState::Conflicting:
      return "conflicting";
  }
  return "unknown";
}

std::string_view to_string(AbiEvidenceScope scope) {
  switch (scope) {
    case AbiEvidenceScope::Toolchain:
      return "toolchain";
    case AbiEvidenceScope::CompileAction:
      return "compile-action";
    case AbiEvidenceScope::TranslationUnit:
      return "translation-unit";
    case AbiEvidenceScope::ObjectFile:
      return "object-file";
    case AbiEvidenceScope::ExportedInterface:
      return "exported-interface";
    case AbiEvidenceScope::LinkedArtifact:
      return "linked-artifact";
  }
  return "toolchain";
}

std::string_view to_string(AbiArgumentClass classification) {
  switch (classification) {
    case AbiArgumentClass::AbiAffecting:
      return "abi-affecting";
    case AbiArgumentClass::AbiNeutral:
      return "abi-neutral";
    case AbiArgumentClass::PotentiallyAbiAffecting:
      return "potentially-abi-affecting";
    case AbiArgumentClass::Unsupported:
      return "unsupported";
    case AbiArgumentClass::Conflicting:
      return "conflicting";
  }
  return "potentially-abi-affecting";
}

namespace {

void add_feature(AbiSignature& signature, std::string feature) {
  signature.abi_affecting_features.push_back(std::move(feature));
}

void replace_feature(AbiSignature& signature, std::string_view prefix, std::string feature) {
  signature.abi_affecting_features.erase(
      std::remove_if(signature.abi_affecting_features.begin(),
                     signature.abi_affecting_features.end(),
                     [prefix](const std::string& existing) {
                       return existing.rfind(prefix, 0) == 0;
                     }),
      signature.abi_affecting_features.end());
  signature.abi_affecting_features.push_back(std::move(feature));
}

void sort_features(AbiSignature& signature) {
  std::sort(signature.abi_affecting_features.begin(),
            signature.abi_affecting_features.end());
  signature.abi_affecting_features.erase(
      std::unique(signature.abi_affecting_features.begin(),
                  signature.abi_affecting_features.end()),
      signature.abi_affecting_features.end());
}

std::pair<std::string, std::string> split_macro(std::string_view text) {
  const std::size_t equals = text.find('=');
  if (equals == std::string_view::npos) {
    return {std::string(text), "1"};
  }
  return {std::string(text.substr(0, equals)), std::string(text.substr(equals + 1))};
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string effect_for_macro(std::string_view macro, std::string_view value) {
  if (macro == "_GLIBCXX_USE_CXX11_ABI") {
    return "_GLIBCXX_USE_CXX11_ABI=" + std::string(value);
  }
  if (macro == "_GLIBCXX_DEBUG") {
    return "_GLIBCXX_DEBUG=1";
  }
  if (macro == "_LIBCPP_ABI_VERSION") {
    return "_LIBCPP_ABI_VERSION=" + std::string(value);
  }
  return std::string();
}

}  // namespace

std::string_view to_string(ThreadModel model) {
  switch (model) {
    case ThreadModel::Posix:
      return "posix";
    case ThreadModel::Win32:
      return "win32";
    case ThreadModel::Single:
      return "single";
    case ThreadModel::Wasm:
      return "wasm";
    case ThreadModel::Unknown:
      break;
  }
  return "unknown";
}

Result<OsVersion> OsVersion::parse(std::string_view text) {
  const std::string trimmed = str::trim(text);
  if (trimmed.empty()) {
    return fail<OsVersion>(Error::make("cova::abi::version", "OS version is empty"));
  }
  auto parts = str::split(trimmed, '.');
  if (parts.empty() || parts.size() > 3) {
    return fail<OsVersion>(
        Error::make("cova::abi::version", "`" + trimmed + "` is not a dotted OS version"));
  }
  OsVersion version;
  int values[3] = {0, 0, 0};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const std::string part = str::trim(parts[i]);
    if (part.empty() ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); })) {
      return fail<OsVersion>(
          Error::make("cova::abi::version", "`" + trimmed + "` has a non-numeric OS version part"));
    }
    try {
      values[i] = std::stoi(part);
    } catch (...) {
      return fail<OsVersion>(Error::make("cova::abi::version", "OS version is out of range"));
    }
  }
  version.major = values[0];
  version.minor = values[1];
  version.patch = values[2];
  return version;
}

std::string OsVersion::to_string() const {
  std::string out = std::to_string(major) + "." + std::to_string(minor);
  if (patch != 0) {
    out += "." + std::to_string(patch);
  }
  return out;
}

std::string AbiSignature::to_string() const {
  std::string out;
  out += triple.to_string();
  out += " abi=";
  out += platform::to_string(abi);
  out += " crt=";
  out += platform::to_string(crt);
  out += " libc++=";
  out += platform::to_string(libcpp);
  out += " link=";
  out += platform::to_string(link_mode);
  out += exceptions ? " exceptions" : " no-exceptions";
  if (exceptions_state != AbiFactState::Known) {
    out += "(" + std::string(cova::platform::to_string(exceptions_state)) + ")";
  }
  out += rtti ? " rtti" : " no-rtti";
  if (rtti_state != AbiFactState::Known) {
    out += "(" + std::string(cova::platform::to_string(rtti_state)) + ")";
  }
  out += " threads=";
  out += platform::to_string(threads);
  if (!stdlib_abi_tag.empty()) {
    out += " stdlib=" + stdlib_abi_tag;
  }
  if (!abi_affecting_features.empty()) {
    std::vector<std::string> sorted = abi_affecting_features;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    out += " features=" + str::join(sorted, ",");
  }
  if (!facts.empty()) {
    out += " facts=" + std::to_string(facts.size());
  }
  return out;
}

std::string AbiSignature::fingerprint_key() const {
  // Every field that can change link compatibility is included. The order is
  // fixed so the key is deterministic across runs and platforms. The schema
  // version is part of the identity: an older key is never reused silently.
  cova::Fingerprint fingerprint;
  fingerprint.add(kAbiSignatureSchemaVersion);
  fingerprint.add(triple.to_string());
  fingerprint.add(platform::to_string(abi));
  fingerprint.add(platform::to_string(crt));
  fingerprint.add(platform::to_string(libcpp));
  fingerprint.add(platform::to_string(link_mode));
  fingerprint.add(std::string("exceptions=") + (exceptions ? "1" : "0") +
                  "/" + std::string(cova::platform::to_string(exceptions_state)));
  fingerprint.add(std::string("rtti=") + (rtti ? "1" : "0") +
                  "/" + std::string(cova::platform::to_string(rtti_state)));
  fingerprint.add(platform::to_string(threads));
  fingerprint.add(stdlib_abi_tag);
  std::vector<std::string> features = abi_affecting_features;
  std::sort(features.begin(), features.end());
  features.erase(std::unique(features.begin(), features.end()), features.end());
  for (const auto& feature : features) {
    fingerprint.add(feature);
  }
  fingerprint.add(abi_facts_fingerprint(facts));
  return fingerprint.finish_hex();
}

namespace {

void note_difference(std::vector<std::string>& differences, std::string_view field,
                     std::string_view left, std::string_view right) {
  differences.emplace_back(std::string(field) + ": `" + std::string(left) + "` vs `" +
                           std::string(right) + "`");
}

bool unknown_value(std::string_view text) { return text == "unknown" || text.empty(); }

}  // namespace

AbiCheckResult check_abi_compatibility(const AbiSignature& left, const AbiSignature& right) {
  AbiCheckResult result;
  bool unknown = false;

  if (left.triple != right.triple) {
    note_difference(result.differences, "target", left.triple.to_string(),
                    right.triple.to_string());
  }
  const std::string_view left_abi = platform::to_string(left.abi);
  const std::string_view right_abi = platform::to_string(right.abi);
  if (left.abi != right.abi) {
    note_difference(result.differences, "abi", left_abi, right_abi);
    unknown = unknown || unknown_value(left_abi) || unknown_value(right_abi);
  }
  const std::string_view left_crt = platform::to_string(left.crt);
  const std::string_view right_crt = platform::to_string(right.crt);
  if (left.crt != right.crt) {
    note_difference(result.differences, "crt", left_crt, right_crt);
    unknown = unknown || unknown_value(left_crt) || unknown_value(right_crt);
  }
  const std::string_view left_libcpp = platform::to_string(left.libcpp);
  const std::string_view right_libcpp = platform::to_string(right.libcpp);
  if (left.libcpp != right.libcpp) {
    note_difference(result.differences, "c++ standard library", left_libcpp, right_libcpp);
    unknown = unknown || unknown_value(left_libcpp) || unknown_value(right_libcpp);
  }
  if (left.link_mode != right.link_mode) {
    note_difference(result.differences, "link mode", platform::to_string(left.link_mode),
                    platform::to_string(right.link_mode));
    if (left.link_mode == LinkMode::Default || right.link_mode == LinkMode::Default) {
      unknown = true;
    }
  }
  if (left.exceptions != right.exceptions) {
    note_difference(result.differences, "exceptions", left.exceptions ? "on" : "off",
                    right.exceptions ? "on" : "off");
  }
  if (left.exceptions_state != right.exceptions_state) {
    unknown = true;
  }
  if (left.rtti != right.rtti) {
    note_difference(result.differences, "rtti", left.rtti ? "on" : "off",
                    right.rtti ? "on" : "off");
  }
  if (left.rtti_state != right.rtti_state) {
    unknown = true;
  }
  const std::string_view left_threads = platform::to_string(left.threads);
  const std::string_view right_threads = platform::to_string(right.threads);
  if (left.threads != right.threads) {
    note_difference(result.differences, "thread model", left_threads, right_threads);
    unknown = unknown || unknown_value(left_threads) || unknown_value(right_threads);
  }
  if (!abi_tags_link_compatible(left.stdlib_abi_tag, right.stdlib_abi_tag)) {
    note_difference(result.differences, "stdlib ABI tag", left.stdlib_abi_tag,
                    right.stdlib_abi_tag);
  }

  std::vector<std::string> left_features = left.abi_affecting_features;
  std::vector<std::string> right_features = right.abi_affecting_features;
  std::sort(left_features.begin(), left_features.end());
  std::sort(right_features.begin(), right_features.end());
  if (left_features != right_features) {
    note_difference(result.differences, "ABI-affecting features", str::join(left_features, ","),
                    str::join(right_features, ","));
  }

  unknown = unknown || left.exceptions_state != AbiFactState::Known ||
            right.exceptions_state != AbiFactState::Known ||
            left.rtti_state != AbiFactState::Known ||
            right.rtti_state != AbiFactState::Known;
  if (result.differences.empty() && !unknown) {
    result.verdict = AbiVerdict::Compatible;
    result.summary = "the two ABI signatures are identical";
    return result;
  }
  result.verdict = unknown ? AbiVerdict::Unknown : AbiVerdict::Incompatible;
  result.summary = unknown ? "compatibility cannot be proven from the available facts"
                           : "the two ABI signatures are incompatible";
  return result;
}

namespace {

bool is_neutral_option(std::string_view argument) {
  static constexpr std::string_view neutral_exact[] = {
      "-c", "-S", "-E", "-o", "-MD", "-MMD", "-MF", "-MT", "-MQ", "-MP",
      "-MG", "-pipe", "-v", "-###", "-pthread", "-pie", "-fPIC", "-fpic",
      "-fPIE", "-fpie", "-fno-pic", "-fno-pie", "-fno-PIC", "-fno-PIE",
  };
  for (const auto option : neutral_exact) {
    if (argument == option) {
      return true;
    }
  }
  static constexpr std::string_view neutral_prefixes[] = {
      "-O", "-g", "-W", "-w", "-pedantic", "-fdiagnostics", "-fcolor",
      "-fno-color-diagnostics", "-fmessage-length", "-fmax-errors",
      "-ffile-prefix-map", "-fmacro-prefix-map", "-fdebug-prefix-map",
      "-I", "-isystem", "-iquote", "-idirafter", "-isysroot", "-imultilib",
      "-iprefix", "-iwithprefix", "-iwithprefixbefore", "-L", "-l", "-Wl,",
      "-Wp,", "-Xlinker", "-Xassembler", "-B",
  };
  for (const auto prefix : neutral_prefixes) {
    if (starts_with(argument, prefix)) {
      return true;
    }
  }
  return false;
}

std::optional<LibCpp> libcpp_from_name(std::string_view name) {
  const std::string lower = str::to_lower(name);
  if (lower == "libc++" || lower == "libcxx") {
    return LibCpp::LibCpp;
  }
  if (lower == "libstdc++" || lower == "libstdcxx") {
    return LibCpp::LibStdCpp;
  }
  if (lower == "platform") {
    return LibCpp::LibCpp;
  }
  return std::nullopt;
}

void apply_argument_effect(AbiSignature& signature, std::string_view effect) {
  const std::string text(effect);
  const auto [name, value] = split_macro(text);
  if (name == "exceptions") {
    signature.exceptions = value != "0";
    signature.exceptions_state = AbiFactState::Known;
    replace_feature(signature, "exceptions=", text);
    return;
  }
  if (name == "rtti") {
    signature.rtti = value != "0";
    signature.rtti_state = AbiFactState::Known;
    replace_feature(signature, "rtti=", text);
    return;
  }
  if (name == "link_mode") {
    if (value == "static") {
      signature.link_mode = LinkMode::Static;
    } else if (value == "shared") {
      signature.link_mode = LinkMode::Dynamic;
    }
    return;
  }
  if (name == "libcpp") {
    if (auto parsed = libcpp_from_name(value)) {
      signature.libcpp = *parsed;
    }
    add_feature(signature, text);
    return;
  }
  add_feature(signature, text);
}

AbiFact make_fact(std::string name, std::string value, AbiFactState state,
                  std::string subject, std::string configuration) {
  AbiFact fact;
  fact.name = std::move(name);
  fact.value = std::move(value);
  fact.state = state;
  fact.scope = AbiEvidenceScope::Toolchain;
  fact.subject = std::move(subject);
  fact.configuration = std::move(configuration);
  fact.source = "probe:preprocessor-macro";
  fact.producer_version = "cova-probe/2";
  return fact;
}

}  // namespace

AbiArgument classify_abi_argument(std::string_view argument) {
  AbiArgument result;
  result.argument = std::string(argument);

  const auto affecting = [&result](std::string effect) {
    result.classification = AbiArgumentClass::AbiAffecting;
    result.effect = std::move(effect);
  };
  const auto neutral = [&result]() {
    result.classification = AbiArgumentClass::AbiNeutral;
    result.effect.clear();
  };
  const auto potentially = [&result]() {
    result.classification = AbiArgumentClass::PotentiallyAbiAffecting;
    result.effect.clear();
  };

  if (argument == "-fno-exceptions") {
    affecting("exceptions=0");
    return result;
  }
  if (argument == "-fexceptions") {
    affecting("exceptions=1");
    return result;
  }
  if (argument == "-fno-rtti") {
    affecting("rtti=0");
    return result;
  }
  if (argument == "-frtti") {
    affecting("rtti=1");
    return result;
  }
  if (argument == "-fshort-enums") {
    affecting("short-enums=1");
    return result;
  }
  if (starts_with(argument, "-fpack-struct")) {
    affecting(argument == "-fpack-struct" ? "pack-struct=1" : std::string(argument));
    return result;
  }
  if (argument == "-fshort-wchar") {
    affecting("short-wchar=1");
    return result;
  }
  if (argument == "-static") {
    affecting("link_mode=static");
    return result;
  }
  if (argument == "-shared") {
    affecting("link_mode=shared");
    return result;
  }
  if (starts_with(argument, "-mabi=")) {
    affecting(std::string(argument));
    return result;
  }
  if (argument == "-m32" || argument == "-m64" || argument == "-mx32") {
    affecting("data-model=" + std::string(argument.substr(1)));
    return result;
  }
  if (argument == "-msoft-float" || argument == "-mhard-float") {
    affecting(std::string(argument));
    return result;
  }
  if (starts_with(argument, "-mfloat-abi=")) {
    affecting(std::string(argument));
    return result;
  }
  if (starts_with(argument, "-stdlib=")) {
    affecting("libcpp=" + std::string(argument.substr(8)));
    return result;
  }
  if (starts_with(argument, "-fabi-version=") || starts_with(argument, "-fclang-abi-compat=")) {
    affecting(std::string(argument));
    return result;
  }
  if (starts_with(argument, "-D")) {
    const std::string macro_text(argument.substr(2));
    const auto [macro, value] = split_macro(macro_text);
    const std::string effect = effect_for_macro(macro, value);
    if (!effect.empty()) {
      affecting(effect);
    } else {
      // Any macro can gate a layout-changing #ifdef, so it is not neutral.
      potentially();
    }
    return result;
  }
  if (argument == "-include" || argument == "-imacros" || starts_with(argument, "-include=") ||
      starts_with(argument, "-imacros=")) {
    potentially();
    return result;
  }
  if (starts_with(argument, "-std=") || starts_with(argument, "-fvisibility=") ||
      starts_with(argument, "-fno-threadsafe-statics") || starts_with(argument, "-fplugin=") ||
      starts_with(argument, "-fsanitize=") || starts_with(argument, "-fno-sanitize=")) {
    potentially();
    return result;
  }
  if (is_neutral_option(argument)) {
    neutral();
    return result;
  }
  if (argument.empty() || argument.front() != '-') {
    // A bare path is an input/output, not a compiler option.
    neutral();
    return result;
  }
  // Unknown flags must not be silently treated as ABI neutral.
  potentially();
  return result;
}

EffectiveAbi effective_abi(const AbiSignature& baseline,
                           const std::vector<std::string>& arguments) {
  EffectiveAbi result;
  result.signature = baseline;
  Fingerprint opaque;
  opaque.add(kAbiSignatureSchemaVersion);

  std::vector<std::string> normalized;
  normalized.reserve(arguments.size());
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "-D" && index + 1 < arguments.size()) {
      normalized.push_back("-D" + arguments[++index]);
    } else {
      normalized.push_back(arguments[index]);
    }
  }

  for (std::size_t index = 0; index < normalized.size(); ++index) {
    AbiArgument argument = classify_abi_argument(normalized[index]);
    if (argument.classification == AbiArgumentClass::AbiAffecting) {
      apply_argument_effect(result.signature, argument.effect);
    } else if (argument.classification != AbiArgumentClass::AbiNeutral) {
      opaque.add(static_cast<int64_t>(index));
      opaque.add(argument.argument);
      if (argument.classification == AbiArgumentClass::Unsupported) {
        result.unsupported.push_back(argument.argument);
        result.state = AbiFactState::Unsupported;
      } else if (argument.classification == AbiArgumentClass::Conflicting) {
        result.state = AbiFactState::Conflicting;
      } else if (result.state == AbiFactState::Known) {
        // Potentially affecting, unproven.
        result.state = AbiFactState::Unknown;
      }
    }
    result.arguments.push_back(std::move(argument));
  }
  sort_features(result.signature);
  result.opaque_fingerprint = opaque.finish_hex();
  return result;
}

std::string EffectiveAbi::identity_key() const {
  Fingerprint fingerprint;
  fingerprint.add(kAbiSignatureSchemaVersion);
  fingerprint.add(kAbiClassifierVersion);
  fingerprint.add(signature.fingerprint_key());
  fingerprint.add(opaque_fingerprint);
  fingerprint.add(std::string(to_string(state)));
  for (const auto& argument : unsupported) {
    fingerprint.add(argument);
  }
  return fingerprint.finish_hex();
}

std::string abi_facts_fingerprint(const std::vector<AbiFact>& facts) {
  std::vector<std::string> lines;
  lines.reserve(facts.size());
  for (const auto& fact : facts) {
    std::string line;
    line += std::string(to_string(fact.scope));
    line += '|';
    line += fact.subject;
    line += '|';
    line += fact.configuration;
    line += '|';
    line += fact.name;
    line += '=';
    line += fact.value;
    line += '|';
    line += std::string(to_string(fact.state));
    line += '|';
    line += fact.source;
    line += '|';
    line += fact.producer_version;
    lines.push_back(std::move(line));
  }
  std::sort(lines.begin(), lines.end());
  Fingerprint fingerprint;
  fingerprint.add(kAbiSignatureSchemaVersion);
  for (const auto& line : lines) {
    fingerprint.add(line);
  }
  return fingerprint.finish_hex();
}

AbiSignature apply_abi_facts(const AbiSignature& baseline, std::string_view compiler_family,
                             const std::map<std::string, std::string, std::less<>>& facts) {
  AbiSignature signature = baseline;
  const std::string family = str::to_lower(compiler_family);
  const std::string subject = baseline.triple.to_string() + "|" + family;
  const std::string configuration = "compiler-default";
  const auto lookup = [&facts](std::string_view name) -> const std::string* {
    auto it = facts.find(name);
    return it == facts.end() ? nullptr : &it->second;
  };

  const bool msvc_like =
      family == "clang-cl" || family.find("msvc") != std::string::npos;
  const bool gcc_or_clang = !msvc_like &&
                            (family.find("gcc") != std::string::npos ||
                             family.find("g++") != std::string::npos ||
                             family.find("clang") != std::string::npos);

  if (gcc_or_clang) {
    if (const std::string* value = lookup("__EXCEPTIONS")) {
      signature.exceptions = *value != "0";
      signature.exceptions_state = AbiFactState::Known;
      signature.facts.push_back(make_fact("__EXCEPTIONS", *value, AbiFactState::Known, subject,
                                          configuration));
    }
    if (const std::string* value = lookup("__GXX_RTTI")) {
      signature.rtti = *value != "0";
      signature.rtti_state = AbiFactState::Known;
      signature.facts.push_back(make_fact("__GXX_RTTI", *value, AbiFactState::Known, subject,
                                          configuration));
    }
  } else {
    if (const std::string* value = lookup("_CPPUNWIND")) {
      signature.exceptions = true;
      signature.exceptions_state = AbiFactState::Known;
      signature.facts.push_back(make_fact("_CPPUNWIND", *value, AbiFactState::Known, subject,
                                          configuration));
    }
    if (const std::string* value = lookup("_CPPRTTI")) {
      signature.rtti = true;
      signature.rtti_state = AbiFactState::Known;
      signature.facts.push_back(make_fact("_CPPRTTI", *value, AbiFactState::Known, subject,
                                          configuration));
    }
  }

  const auto add_known_fact = [&](std::string_view macro, std::string_view feature) {
    if (const std::string* value = lookup(macro)) {
      signature.facts.push_back(make_fact(std::string(macro), *value, AbiFactState::Known,
                                          subject, configuration));
      add_feature(signature, std::string(feature));
    }
  };
  add_known_fact("_GLIBCXX_USE_CXX11_ABI",
                 "_GLIBCXX_USE_CXX11_ABI=" + (lookup("_GLIBCXX_USE_CXX11_ABI")
                                                 ? *lookup("_GLIBCXX_USE_CXX11_ABI")
                                                 : std::string()));
  add_known_fact("_GLIBCXX_DEBUG", "_GLIBCXX_DEBUG=1");
  add_known_fact("_LIBCPP_ABI_VERSION",
                 "_LIBCPP_ABI_VERSION=" + (lookup("_LIBCPP_ABI_VERSION")
                                               ? *lookup("_LIBCPP_ABI_VERSION")
                                               : std::string()));
  add_known_fact("__GXX_ABI_VERSION",
                 "__GXX_ABI_VERSION=" + (lookup("__GXX_ABI_VERSION")
                                             ? *lookup("__GXX_ABI_VERSION")
                                             : std::string()));
  if (const std::string* name = lookup("_LIBCPP_ABI_NAMESPACE")) {
    signature.stdlib_abi_tag = *name;
  }

  static constexpr std::string_view layout_macros[] = {
      "__SIZEOF_POINTER__", "__SIZEOF_LONG__", "__SIZEOF_INT__", "__SIZEOF_WCHAR_T__",
      "__SIZEOF_SIZE_T__", "__BYTE_ORDER__", "__CHAR_UNSIGNED__",
  };
  for (const auto macro : layout_macros) {
    if (const std::string* value = lookup(macro)) {
      signature.facts.push_back(make_fact(std::string(macro), *value, AbiFactState::Known,
                                          subject, configuration));
      add_feature(signature, "data-layout/" + std::string(macro) + "=" + *value);
    }
  }
  sort_features(signature);
  return signature;
}

LibCpp default_libcpp_for(std::string_view compiler_family, const Triple& triple) {
  const std::string family = str::to_lower(compiler_family);
  if (triple.os == Os::Windows) {
    return family == "msvc" || family == "clang-cl" || triple.env == Env::Msvc
               ? LibCpp::MsvcStl
               : LibCpp::LibStdCpp;
  }
  if (triple.os == Os::Wasi || triple.os == Os::Bare) {
    return LibCpp::NoStd;
  }
  if (family == "clang" && (triple.os == Os::Macos || triple.os == Os::Ios)) {
    return LibCpp::LibCpp;
  }
  if (family == "apple-clang") {
    return LibCpp::LibCpp;
  }
  return LibCpp::LibStdCpp;
}

Crt default_crt_for(const Triple& triple) { return triple.crt(); }

bool abi_tags_link_compatible(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) {
    // An unrecorded tag does not prove incompatibility; it just cannot be used
    // to prove compatibility either. Callers that need certainty must compare
    // the full signatures.
    return true;
  }
  return a == b;
}

bool is_abi_affecting_feature(std::string_view feature) {
  const std::string lower = str::to_lower(feature);
  static const std::string_view markers[] = {
      "_glibcxx_use_cxx11_abi",
      "_glibcxx_debug",
      "_glibcxx_assertions",
      "_libcpp_abi",
      "stdlib=",
      "std=",
      "fno-exceptions",
      "fno-rtti",
      "fno-threadsafe-statics",
      "fshort-wchar",
      "fshort-enums",
      "fpack-struct",
      "fabi-version",
      "fclang-abi-compat",
      "exceptions=",
      "rtti=",
      "short-enums",
      "pack-struct",
      "short-wchar",
      "data-layout/",
      "data-model",
      "mabi=",
      "msse",
      "mavx",
  };
  for (const auto marker : markers) {
    if (lower.find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace cova::platform
