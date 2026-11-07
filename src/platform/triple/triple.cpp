// Cova — structured target triples (PRD §6.2, TGT-001/002/007).
#include "platform/triple/triple.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace cova::platform {
namespace {

constexpr std::string_view kUnknown = "unknown";

std::optional<Arch> normalised_arch(std::string_view text) {
  if (auto arch = arch_from_string(text)) {
    return arch;
  }
  return std::nullopt;
}

bool is_vendor_component(std::string_view text) {
  return vendor_from_string(text).has_value();
}

bool is_os_component(std::string_view text) {
  return os_from_string(text).has_value();
}

// Fills the OS-dependent defaults that make a triple complete enough for the
// resolver: ABI, CRT, stdlib and the conventional environment.
void apply_os_defaults(Triple& triple) {
  if (triple.os == Os::Windows) {
    if (triple.env == Env::Unknown) {
      // A Windows triple without an environment is rare; MSVC is the default
      // assumption only when the vendor says "pc" and the ABI suggests MSVC.
      triple.env = Env::Msvc;
    }
  }
  // Other operating systems keep an unknown environment rather than gaining a
  // synthetic `none` component; `wasi` and `none` already name the runtime.
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum <-> string
// ---------------------------------------------------------------------------
std::string_view to_string(Arch arch) {
  switch (arch) {
    case Arch::X86:
      return "i386";
    case Arch::X86_64:
      return "x86_64";
    case Arch::Aarch64:
      return "aarch64";
    case Arch::Arm:
      return "arm";
    case Arch::Riscv64:
      return "riscv64";
    case Arch::Riscv32:
      return "riscv32";
    case Arch::Wasm32:
      return "wasm32";
    case Arch::Wasm64:
      return "wasm64";
    case Arch::Mips64:
      return "mips64";
    case Arch::PowerPc64:
      return "powerpc64";
    case Arch::LoongArch64:
      return "loongarch64";
    case Arch::Unknown:
      break;
  }
  return kUnknown;
}

std::string_view to_string(Vendor vendor) {
  switch (vendor) {
    case Vendor::Pc:
      return "pc";
    case Vendor::Apple:
      return "apple";
    case Vendor::Linux:
      return "linux";
    case Vendor::Nvidia:
      return "nvidia";
    case Vendor::Ibm:
      return "ibm";
    case Vendor::Amd:
      return "amd";
    case Vendor::Espressif:
      return "espressif";
    case Vendor::Unknown:
    case Vendor::UnknownVendor:
      break;
  }
  return kUnknown;
}

std::string_view to_string(Os os) {
  switch (os) {
    case Os::Linux:
      return "linux";
    case Os::Windows:
      return "windows";
    case Os::Macos:
      return "macos";
    case Os::FreeBsd:
      return "freebsd";
    case Os::NetBsd:
      return "netbsd";
    case Os::OpenBsd:
      return "openbsd";
    case Os::Wasi:
      return "wasi";
    case Os::Bare:
      return "none";
    case Os::Android:
      return "android";
    case Os::Ios:
      return "ios";
    case Os::Unknown:
      break;
  }
  return kUnknown;
}

std::string_view to_string(Env env) {
  switch (env) {
    case Env::Gnu:
      return "gnu";
    case Env::Musl:
      return "musl";
    case Env::Msvc:
      return "msvc";
    case Env::Gnueabi:
      return "gnueabi";
    case Env::Gnueabihf:
      return "gnueabihf";
    case Env::Uclibc:
      return "uclibc";
    case Env::Eabi:
      return "eabi";
    case Env::Eigen:
      return "eigen";
    case Env::Sim:
      return "sim";
    case Env::None:
      return "none";
    case Env::Unknown:
      break;
  }
  return kUnknown;
}

std::string_view to_string(Abi abi) {
  switch (abi) {
    case Abi::SysV:
      return "sysv";
    case Abi::Msvc:
      return "msvc";
    case Abi::Gnueabi:
      return "gnueabi";
    case Abi::Gnueabihf:
      return "gnueabihf";
    case Abi::Musl:
      return "musl";
    case Abi::Wasm:
      return "wasm";
    case Abi::Eabi:
      return "eabi";
    case Abi::MachO:
      return "macho";
    case Abi::Unknown:
      break;
  }
  return kUnknown;
}

std::string_view to_string(Crt crt) {
  switch (crt) {
    case Crt::Glibc:
      return "glibc";
    case Crt::Musl:
      return "musl";
    case Crt::Uclibc:
      return "uclibc";
    case Crt::Msvc:
      return "msvc";
    case Crt::WasmWasi:
      return "wasi";
    case Crt::None:
      return "none";
    case Crt::Unknown:
      break;
  }
  return kUnknown;
}

std::string_view to_string(LibCpp libcpp) {
  switch (libcpp) {
    case LibCpp::LibStdCpp:
      return "libstdc++";
    case LibCpp::LibCpp:
      return "libc++";
    case LibCpp::MsvcStl:
      return "msvc-stl";
    case LibCpp::NoStd:
      return "none";
    case LibCpp::Unknown:
      break;
  }
  return kUnknown;
}

std::string_view to_string(LinkMode mode) {
  switch (mode) {
    case LinkMode::Static:
      return "static";
    case LinkMode::Dynamic:
      return "dynamic";
    case LinkMode::Default:
      return "default";
  }
  return "default";
}

std::optional<Arch> arch_from_string(std::string_view s) {
  const std::string lower = str::to_lower(str::trim_view(s));
  if (lower == "x86_64" || lower == "x86-64" || lower == "amd64" || lower == "x64") {
    return Arch::X86_64;
  }
  if (lower == "i386" || lower == "i486" || lower == "i586" || lower == "i686" ||
      lower == "x86" || lower == "ia32") {
    return Arch::X86;
  }
  if (lower == "aarch64" || lower == "arm64" || lower == "armv8" || lower == "armv8a") {
    return Arch::Aarch64;
  }
  if (lower == "arm" || lower == "armv7" || lower == "armv7l" || lower == "thumbv7" ||
      lower == "armv6" || lower == "armhf" || lower == "armel") {
    return Arch::Arm;
  }
  if (lower == "riscv64" || lower == "riscv64gc" || lower == "riscv64imac") {
    return Arch::Riscv64;
  }
  if (lower == "riscv32" || lower == "riscv32imac") {
    return Arch::Riscv32;
  }
  if (lower == "wasm32") {
    return Arch::Wasm32;
  }
  if (lower == "wasm64") {
    return Arch::Wasm64;
  }
  if (lower == "mips64" || lower == "mips64el") {
    return Arch::Mips64;
  }
  if (lower == "powerpc64" || lower == "ppc64" || lower == "ppc64le" ||
      lower == "powerpc64le") {
    return Arch::PowerPc64;
  }
  if (lower == "loongarch64" || lower == "loong64") {
    return Arch::LoongArch64;
  }
  return std::nullopt;
}

std::optional<Os> os_from_string(std::string_view s) {
  const std::string lower = str::to_lower(str::trim_view(s));
  if (lower == "linux") {
    return Os::Linux;
  }
  if (lower == "windows" || lower == "win32" || lower == "win") {
    return Os::Windows;
  }
  if (lower == "macos" || lower == "macosx" || lower == "darwin" || lower == "osx") {
    return Os::Macos;
  }
  if (lower == "freebsd") {
    return Os::FreeBsd;
  }
  if (lower == "netbsd") {
    return Os::NetBsd;
  }
  if (lower == "openbsd") {
    return Os::OpenBsd;
  }
  if (lower == "wasi" || lower == "wasip1" || lower == "wasip2") {
    return Os::Wasi;
  }
  if (lower == "none" || lower == "bare" || lower == "bare-metal" ||
      lower == "unknown-none") {
    return Os::Bare;
  }
  if (lower == "android" || lower == "androideabi") {
    return Os::Android;
  }
  if (lower == "ios") {
    return Os::Ios;
  }
  return std::nullopt;
}

std::optional<Env> env_from_string(std::string_view s) {
  const std::string lower = str::to_lower(str::trim_view(s));
  if (lower == "gnu") {
    return Env::Gnu;
  }
  if (lower == "musl" || lower == "musleabi" || lower == "musleabihf") {
    return Env::Musl;
  }
  if (lower == "msvc") {
    return Env::Msvc;
  }
  if (lower == "gnueabi" || lower == "androideabi") {
    return Env::Gnueabi;
  }
  if (lower == "gnueabihf" || lower == "eabihf" || lower == "androideabihf") {
    return Env::Gnueabihf;
  }
  if (lower == "uclibc") {
    return Env::Uclibc;
  }
  if (lower == "eabi" || lower == "elf") {
    return Env::Eabi;
  }
  if (lower == "eigen") {
    return Env::Eigen;
  }
  if (lower == "sim") {
    return Env::Sim;
  }
  if (lower == "none" || lower == "unknown") {
    return Env::None;
  }
  return std::nullopt;
}

std::optional<Vendor> vendor_from_string(std::string_view s) {
  const std::string lower = str::to_lower(str::trim_view(s));
  if (lower == "unknown" || lower == "none") {
    return Vendor::UnknownVendor;
  }
  if (lower == "pc" || lower == "w64") {
    return Vendor::Pc;
  }
  if (lower == "apple") {
    return Vendor::Apple;
  }
  if (lower == "linux") {
    return Vendor::Linux;
  }
  if (lower == "nvidia") {
    return Vendor::Nvidia;
  }
  if (lower == "ibm") {
    return Vendor::Ibm;
  }
  if (lower == "amd") {
    return Vendor::Amd;
  }
  if (lower == "espressif" || lower == "esp") {
    return Vendor::Espressif;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Triple
// ---------------------------------------------------------------------------
Result<Triple> Triple::parse(std::string_view text) {
  const std::string trimmed = str::trim(text);
  if (trimmed.empty()) {
    return fail<Triple>(Error::make("cova::target::empty", "target triple is empty"));
  }
  auto parts = str::split(str::to_lower(trimmed), '-');
  if (parts.size() < 2 || parts.size() > 4) {
    return fail<Triple>(
        Error::make("cova::target::invalid",
                    "`" + trimmed + "` is not a valid target triple")
            .with("expected 2 to 4 hyphen-separated components")
            .with("example: x86_64-unknown-linux-gnu"));
  }

  Triple triple;
  auto arch = normalised_arch(parts[0]);
  if (!arch) {
    return fail<Triple>(
        Error::make("cova::target::arch_unknown",
                    "unknown target architecture `" + parts[0] + "`")
            .with("supported: x86_64, aarch64, arm, riscv64, wasm32, ..."));
  }
  triple.arch = *arch;

  if (parts.size() == 2) {
    auto os = os_from_string(parts[1]);
    if (!os) {
      return fail<Triple>(
          Error::make("cova::target::os_unknown", "unknown target OS `" + parts[1] + "`")
              .with("supported: linux, windows, macos, wasi, none, ..."));
    }
    triple.os = *os;
  } else if (parts.size() == 3) {
    const bool vendor_second = is_vendor_component(parts[1]);
    const bool os_second = is_os_component(parts[1]);
    const bool os_third = is_os_component(parts[2]);
    const bool env_third = env_from_string(parts[2]).has_value();

    if (vendor_second && os_third) {
      triple.vendor = *vendor_from_string(parts[1]);
      triple.os = *os_from_string(parts[2]);
    } else if (os_second && env_third) {
      triple.os = *os_from_string(parts[1]);
      triple.env = *env_from_string(parts[2]);
    } else if (os_second) {
      triple.os = *os_from_string(parts[1]);
    } else if (vendor_second && env_third) {
      triple.vendor = *vendor_from_string(parts[1]);
      triple.env = *env_from_string(parts[2]);
    } else {
      return fail<Triple>(
          Error::make("cova::target::invalid",
                      "cannot interpret `" + trimmed + "` as arch-vendor-os or arch-os-env"));
    }
  } else {
    auto vendor = vendor_from_string(parts[1]);
    auto os = os_from_string(parts[2]);
    auto env = env_from_string(parts[3]);
    if (!vendor) {
      return fail<Triple>(
          Error::make("cova::target::vendor_unknown", "unknown target vendor `" + parts[1] + "`"));
    }
    if (!os) {
      return fail<Triple>(
          Error::make("cova::target::os_unknown", "unknown target OS `" + parts[2] + "`"));
    }
    if (!env) {
      return fail<Triple>(
          Error::make("cova::target::env_unknown", "unknown target environment `" + parts[3] + "`"));
    }
    triple.vendor = *vendor;
    triple.os = *os;
    triple.env = *env;
  }

  // Vendor normalisation: `pc` and `unknown` are the same vendor for Linux, and
  // Linux itself is not a vendor in LLVM's model.
  if (triple.os == Os::Linux && (triple.vendor == Vendor::Pc || triple.vendor == Vendor::Linux)) {
    triple.vendor = Vendor::UnknownVendor;
  }
  if (triple.vendor == Vendor::Unknown) {
    triple.vendor = Vendor::UnknownVendor;
  }
  apply_os_defaults(triple);

  if (!triple.is_valid()) {
    return fail<Triple>(
        Error::make("cova::target::invalid", "target `" + trimmed + "` is incomplete")
            .with("missing an architecture, operating system or environment"));
  }
  return triple;
}

std::string Triple::to_string() const {
  std::string out;
  out.append(platform::to_string(arch));
  out.push_back('-');
  out.append(platform::to_string(vendor));
  out.push_back('-');
  out.append(platform::to_string(os));
  if (env != Env::Unknown) {
    out.push_back('-');
    out.append(platform::to_string(env));
  }
  return out;
}

std::string Triple::to_short_string() const {
  std::string out;
  out.append(platform::to_string(arch));
  if (vendor != Vendor::UnknownVendor && vendor != Vendor::Unknown) {
    out.push_back('-');
    out.append(platform::to_string(vendor));
  }
  out.push_back('-');
  out.append(platform::to_string(os));
  if (env != Env::Unknown && env != Env::None) {
    out.push_back('-');
    out.append(platform::to_string(env));
  } else if (env == Env::None && (os == Os::Wasi || os == Os::Bare)) {
    // `wasi`/`none` already identify the environment.
  }
  return out;
}

std::string Triple::to_display_string() const {
  std::string os_label;
  switch (os) {
    case Os::Linux:
      os_label = "Linux";
      break;
    case Os::Windows:
      os_label = "Windows";
      break;
    case Os::Macos:
      os_label = "macOS";
      break;
    case Os::FreeBsd:
      os_label = "FreeBSD";
      break;
    case Os::NetBsd:
      os_label = "NetBSD";
      break;
    case Os::OpenBsd:
      os_label = "OpenBSD";
      break;
    case Os::Wasi:
      os_label = "WASI";
      break;
    case Os::Bare:
      os_label = "bare metal";
      break;
    case Os::Android:
      os_label = "Android";
      break;
    case Os::Ios:
      os_label = "iOS";
      break;
    case Os::Unknown:
      os_label = "unknown OS";
      break;
  }
  std::string out = os_label + " " + std::string(platform::to_string(arch));
  if (env != Env::Unknown && env != Env::None) {
    std::string env_label = str::to_upper(std::string(platform::to_string(env)));
    out += " (" + env_label + ")";
  }
  return out;
}

bool Triple::is_valid() const {
  return arch != Arch::Unknown && os != Os::Unknown;
}

bool Triple::is_native_like() const {
  const Triple& host = host_triple();
  if (arch != host.arch || os != host.os) {
    return false;
  }
  if (env == Env::Unknown || env == Env::None) {
    return true;
  }
  return env == host.env;
}

Abi Triple::abi() const {
  if (os == Os::Windows) {
    return env == Env::Msvc ? Abi::Msvc : Abi::SysV;
  }
  if (os == Os::Wasi) {
    return Abi::Wasm;
  }
  if (os == Os::Macos || os == Os::Ios) {
    return Abi::MachO;
  }
  if (os == Os::Bare) {
    return Abi::Eabi;
  }
  if (env == Env::Musl) {
    return Abi::Musl;
  }
  if (env == Env::Gnueabihf) {
    return Abi::Gnueabihf;
  }
  if (env == Env::Gnueabi || env == Env::Eabi || env == Env::Eigen) {
    return Abi::Gnueabi;
  }
  if (os == Os::Linux || os == Os::Android || os == Os::FreeBsd || os == Os::NetBsd ||
      os == Os::OpenBsd) {
    return Abi::SysV;
  }
  return Abi::Unknown;
}

Crt Triple::crt() const {
  if (os == Os::Windows) {
    return env == Env::Msvc ? Crt::Msvc : Crt::Unknown;
  }
  if (os == Os::Wasi) {
    return Crt::WasmWasi;
  }
  if (os == Os::Bare) {
    return Crt::None;
  }
  if (env == Env::Musl) {
    return Crt::Musl;
  }
  if (env == Env::Uclibc) {
    return Crt::Uclibc;
  }
  if (env == Env::Gnu || env == Env::Gnueabi || env == Env::Gnueabihf) {
    return Crt::Glibc;
  }
  if (os == Os::Linux || os == Os::Android) {
    return Crt::Glibc;
  }
  return Crt::Unknown;
}

LibCpp Triple::default_libcpp() const {
  if (os == Os::Windows) {
    return env == Env::Msvc ? LibCpp::MsvcStl : LibCpp::LibStdCpp;
  }
  if (os == Os::Wasi || os == Os::Bare) {
    return LibCpp::NoStd;
  }
  if (os == Os::Macos || os == Os::Ios) {
    return LibCpp::LibCpp;
  }
  return LibCpp::LibStdCpp;
}

std::string Triple::executable_suffix() const { return os == Os::Windows ? ".exe" : ""; }

std::string Triple::static_library_prefix() const {
  return os == Os::Windows && env == Env::Msvc ? "" : "lib";
}

std::string Triple::static_library_suffix() const {
  return os == Os::Windows && env == Env::Msvc ? ".lib" : ".a";
}

std::string Triple::object_suffix() const {
  return os == Os::Windows && env == Env::Msvc ? ".obj" : ".o";
}

// ---------------------------------------------------------------------------
// Host and known targets
// ---------------------------------------------------------------------------
const Triple& host_triple() {
  static const Triple host = [] {
    Triple triple;
#if defined(__x86_64__) || defined(_M_X64)
    triple.arch = Arch::X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    triple.arch = Arch::Aarch64;
#elif defined(__i386__) || defined(_M_IX86)
    triple.arch = Arch::X86;
#elif defined(__arm__) || defined(_M_ARM)
    triple.arch = Arch::Arm;
#elif defined(__riscv) && (__riscv_xlen == 64)
    triple.arch = Arch::Riscv64;
#else
    triple.arch = Arch::Unknown;
#endif

#if defined(_WIN32)
    triple.os = Os::Windows;
    triple.env = Env::Msvc;
#elif defined(__APPLE__)
    triple.os = Os::Macos;
    triple.env = Env::None;
#elif defined(__linux__)
    triple.os = Os::Linux;
    triple.vendor = Vendor::UnknownVendor;
    triple.env = Env::Gnu;
#elif defined(__FreeBSD__)
    triple.os = Os::FreeBsd;
    triple.env = Env::None;
#elif defined(__wasi__)
    triple.os = Os::Wasi;
    triple.env = Env::None;
#else
    triple.os = Os::Unknown;
#endif
    triple.vendor = Vendor::UnknownVendor;
    return triple;
  }();
  return host;
}

const Triple& build_host_triple() { return host_triple(); }

const std::vector<Triple>& known_targets() {
  static const std::vector<Triple> targets = [] {
    std::vector<std::string> names = {
        // Milestone 2 MVP.
        "x86_64-unknown-linux-gnu",
        "aarch64-unknown-linux-musl",
        // Phase-two platforms (PRD §20.2).
        "aarch64-unknown-linux-gnu",
        "x86_64-pc-windows-gnu",
        "wasm32-unknown-wasi",
        "x86_64-apple-darwin",
        "aarch64-apple-darwin",
        "riscv64-unknown-linux-gnu",
        // Phase-three targets that the model already understands.
        "x86_64-pc-windows-msvc",
        "aarch64-linux-android",
        "x86_64-unknown-freebsd",
        "aarch64-unknown-none",
    };
    std::vector<Triple> out;
    out.reserve(names.size());
    for (const auto& name : names) {
      auto parsed = Triple::parse(name);
      if (parsed) {
        out.push_back(*parsed);
      }
    }
    return out;
  }();
  return targets;
}

Result<Triple> resolve_target(std::string_view text) {
  auto parsed = Triple::parse(text);
  if (!parsed) {
    return parsed;
  }
  for (const auto& known : known_targets()) {
    if (known == *parsed) {
      return known;
    }
  }
  if (*parsed == host_triple()) {
    return *parsed;
  }
  return fail<Triple>(
      Error::make("cova::target::unsupported",
                  "target `" + parsed->to_string() + "` is not in the supported target list")
          .with("run `cova target list` to see the targets this build recognises")
          .with("PRD §6.4 allows user-defined target specs; this build does not yet accept them"));
}

bool is_known_target(std::string_view text) { return resolve_target(text).has_value(); }

std::vector<std::string> known_target_names() {
  std::vector<std::string> names;
  names.reserve(known_targets().size());
  for (const auto& target : known_targets()) {
    names.push_back(target.to_string());
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

}  // namespace cova::platform
