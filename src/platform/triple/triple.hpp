// Cova — structured target triples (PRD §6.2, TGT-001/002/007).
//
// A triple is parsed once into structured fields. Business logic must never
// re-parse the string. `to_string()` is the canonical stable serialisation used
// by lockfiles, cache keys and directory names.
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.hpp"

namespace cova::platform {

enum class Arch {
  Unknown,
  X86,        // i386/i486/i586/i686
  X86_64,
  Aarch64,
  Arm,        // armv7 etc.
  Riscv64,
  Riscv32,
  Wasm32,
  Wasm64,
  Mips64,
  PowerPc64,
  LoongArch64,
};

enum class Vendor { Unknown, UnknownVendor, Pc, Apple, Linux, Nvidia, Ibm, Amd, Espressif };

enum class Os { Unknown, Linux, Windows, Macos, FreeBsd, NetBsd, OpenBsd, Wasi, Bare, Android, Ios };

enum class Env { Unknown, Gnu, Musl, Msvc, Gnueabi, Gnueabihf, Uclibc, Eabi, Eigen, Sim, None };

enum class Abi {
  Unknown,
  SysV,
  Msvc,
  Gnueabi,
  Gnueabihf,
  Musl,
  Wasm,
  Eabi,
  MachO,
};

enum class Crt { Unknown, Glibc, Musl, Uclibc, Msvc, WasmWasi, None };
enum class LibCpp { Unknown, LibStdCpp, LibCpp, MsvcStl, NoStd };
enum class LinkMode { Default, Static, Dynamic };

std::string_view to_string(Arch arch);
std::string_view to_string(Vendor vendor);
std::string_view to_string(Os os);
std::string_view to_string(Env env);
std::string_view to_string(Abi abi);
std::string_view to_string(Crt crt);
std::string_view to_string(LibCpp libcpp);
std::string_view to_string(LinkMode mode);

std::optional<Arch> arch_from_string(std::string_view s);
std::optional<Os> os_from_string(std::string_view s);
std::optional<Env> env_from_string(std::string_view s);
std::optional<Vendor> vendor_from_string(std::string_view s);

// The identity of a target, independent of any toolchain (PRD §6.1).
struct Triple {
  Arch arch = Arch::Unknown;
  Vendor vendor = Vendor::UnknownVendor;
  Os os = Os::Unknown;
  Env env = Env::Unknown;
  std::string os_version;  // optional minimum deployment target, e.g. "11.0"

  // Parses and normalises. Accepts 2, 3 and 4 component forms, plus the
  // canonical aliases (x86_64-pc-linux-gnu == x86_64-unknown-linux-gnu).
  static Result<Triple> parse(std::string_view text);

  // Canonical string form. Stable across releases; used in cache keys.
  std::string to_string() const;

  // Component form used for directories: "x86_64-linux-gnu" (vendor dropped
  // when it is unknown).
  std::string to_short_string() const;
  // Human label used in diagnostics: "Linux x86_64 (GNU)".
  std::string to_display_string() const;

  bool is_valid() const;
  // True when the target can run on this build host without an emulator.
  bool is_native_like() const;

  Abi abi() const;
  Crt crt() const;
  LibCpp default_libcpp() const;
  // Executable file extension including the dot ("" on POSIX).
  std::string executable_suffix() const;
  std::string static_library_prefix() const;
  std::string static_library_suffix() const;
  // Default object file extension, e.g. ".o" or ".obj".
  std::string object_suffix() const;

  auto operator<=>(const Triple&) const = default;
  bool operator==(const Triple&) const = default;
};

// The triple describing the machine Cova itself is running on.
const Triple& host_triple();
// Build host triple (distinct from host in principle; equal for native builds).
const Triple& build_host_triple();

// All targets Cova claims first-class support for (PRD §20).
const std::vector<Triple>& known_targets();
// Resolves a user-supplied target string against the known list, applying
// aliases. Unknown targets produce a diagnostic-ready Error.
Result<Triple> resolve_target(std::string_view text);
// True when `text` resolves to a known target.
bool is_known_target(std::string_view text);
// Lists targets as their canonical strings, sorted.
std::vector<std::string> known_target_names();

}  // namespace cova::platform
