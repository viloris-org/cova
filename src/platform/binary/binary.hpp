// Cova — binary artifact inspection (PRD §22.4).
//
// Cross-compilation is only trustworthy if Cova checks what it produced. This
// header parses the few header bytes needed to identify an artifact's format
// and architecture without depending on an external `file` or `objdump`.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/common.hpp"
#include "platform/triple/triple.hpp"

namespace cova::platform {

enum class BinaryFormat { Unknown, Elf, Pe, MachO, Wasm, Archive };

std::string_view to_string(BinaryFormat format);

struct BinaryInfo {
  BinaryFormat format = BinaryFormat::Unknown;
  Arch arch = Arch::Unknown;
  int bit_width = 0;
  std::string detail; // e.g. "ELF", "PE/COFF", "Mach-O", "WebAssembly"

  bool known() const {
    return format != BinaryFormat::Unknown && arch != Arch::Unknown;
  }
  std::string to_string() const;
};

// Reads only the leading bytes needed for identification. A missing or
// unreadable file is an error; a recognised container with an unknown machine
// type is not (the caller can render that honestly).
Result<BinaryInfo> inspect_binary(const std::filesystem::path &path);

// True when the artifact's architecture matches the requested target. Format is
// checked where the target implies one (WASI => WebAssembly).
bool binary_matches_target(const BinaryInfo &info, const Triple &target);

} // namespace cova::platform
