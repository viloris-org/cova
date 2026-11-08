// Cova — ABI model and compatibility rules (PRD §6.3, §8.5, §10.5).
#include "platform/abi/abi.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "core/hashing/hash.hpp"
namespace cova::platform {

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
  out += rtti ? " rtti" : " no-rtti";
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
  return out;
}

std::string AbiSignature::fingerprint_key() const {
  // Every field that can change link compatibility is included. The order is
  // fixed so the key is deterministic across runs and platforms.
  cova::Fingerprint fingerprint;
  fingerprint.add(triple.to_string());
  fingerprint.add(platform::to_string(abi));
  fingerprint.add(platform::to_string(crt));
  fingerprint.add(platform::to_string(libcpp));
  fingerprint.add(platform::to_string(link_mode));
  fingerprint.add(exceptions ? "exceptions=1" : "exceptions=0");
  fingerprint.add(rtti ? "rtti=1" : "rtti=0");
  fingerprint.add(platform::to_string(threads));
  fingerprint.add(stdlib_abi_tag);
  std::vector<std::string> features = abi_affecting_features;
  std::sort(features.begin(), features.end());
  features.erase(std::unique(features.begin(), features.end()), features.end());
  for (const auto& feature : features) {
    fingerprint.add(feature);
  }
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
  if (left.rtti != right.rtti) {
    note_difference(result.differences, "rtti", left.rtti ? "on" : "off",
                    right.rtti ? "on" : "off");
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

  if (result.differences.empty()) {
    result.verdict = AbiVerdict::Compatible;
    result.summary = "the two ABI signatures are identical";
    return result;
  }
  result.verdict = unknown ? AbiVerdict::Unknown : AbiVerdict::Incompatible;
  result.summary = unknown ? "compatibility cannot be proven from the available facts"
                           : "the two ABI signatures are incompatible";
  return result;
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
      "fabi-version",
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
