// Cova — binary artifact inspection (PRD §22.4).
#include "platform/binary/binary.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

namespace cova::platform {
namespace {

std::uint16_t read_u16_le(const std::array<unsigned char, 64> &bytes,
                          std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint16_t read_u16_be(const std::array<unsigned char, 64> &bytes,
                          std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] << 8) |
         static_cast<std::uint16_t>(bytes[offset + 1]);
}

std::uint32_t read_u32_le(const std::array<unsigned char, 64> &bytes,
                          std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t read_u32_be(const std::array<unsigned char, 64> &bytes,
                          std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

Arch elf_machine_to_arch(std::uint16_t machine) {
  switch (machine) {
  case 3:
    return Arch::X86;
  case 8:
    return Arch::Mips64;
  case 20:
    return Arch::PowerPc64;
  case 40:
    return Arch::Arm;
  case 62:
    return Arch::X86_64;
  case 183:
    return Arch::Aarch64;
  case 243:
    return Arch::Riscv64;
  case 258:
    return Arch::LoongArch64;
  default:
    return Arch::Unknown;
  }
}

Arch pe_machine_to_arch(std::uint16_t machine) {
  switch (machine) {
  case 0x014c:
    return Arch::X86;
  case 0x8664:
    return Arch::X86_64;
  case 0xaa64:
    return Arch::Aarch64;
  case 0x01c0:
  case 0x01c4:
    return Arch::Arm;
  default:
    return Arch::Unknown;
  }
}

Arch macho_cpu_to_arch(std::uint32_t cpu) {
  switch (cpu) {
  case 7:
    return Arch::X86;
  case 0x01000007:
    return Arch::X86_64;
  case 12:
    return Arch::Arm;
  case 0x0100000c:
    return Arch::Aarch64;
  default:
    return Arch::Unknown;
  }
}

} // namespace

std::string_view to_string(BinaryFormat format) {
  switch (format) {
  case BinaryFormat::Elf:
    return "ELF";
  case BinaryFormat::Pe:
    return "PE/COFF";
  case BinaryFormat::MachO:
    return "Mach-O";
  case BinaryFormat::Wasm:
    return "WebAssembly";
  case BinaryFormat::Archive:
    return "archive";
  case BinaryFormat::Unknown:
    break;
  }
  return "unknown";
}

std::string BinaryInfo::to_string() const {
  std::string out = std::string(cova::platform::to_string(format));
  if (arch != Arch::Unknown) {
    out += " ";
    out += platform::to_string(arch);
  }
  if (bit_width != 0) {
    out += " " + std::to_string(bit_width) + "-bit";
  }
  if (!detail.empty()) {
    out += " (" + detail + ")";
  }
  return out;
}

Result<BinaryInfo> inspect_binary(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return fail<BinaryInfo>(
        Error::make("cova::binary::unreadable",
                    "cannot read artifact " + path.generic_string()));
  }
  std::array<unsigned char, 64> bytes{};
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  const std::streamsize count = stream.gcount();
  if (count < 4) {
    return fail<BinaryInfo>(Error::make("cova::binary::too_short",
                                        "artifact is too short to identify: " +
                                            path.generic_string()));
  }

  BinaryInfo info;
  // ELF: 7f 45 4c 46.
  if (bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
      bytes[3] == 'F') {
    info.format = BinaryFormat::Elf;
    info.detail = "ELF";
    const bool is_64 = bytes[4] == 2;
    info.bit_width = is_64 ? 64 : 32;
    const bool big_endian = bytes[5] == 2;
    const std::uint16_t machine =
        big_endian ? read_u16_be(bytes, 18) : read_u16_le(bytes, 18);
    info.arch = elf_machine_to_arch(machine);
    return info;
  }

  // PE/COFF: MZ ... e_lfanew -> "PE\0\0" then machine.
  if (bytes[0] == 'M' && bytes[1] == 'Z') {
    if (count < 64) {
      return fail<BinaryInfo>(Error::make("cova::binary::too_short",
                                          "PE artifact header is truncated"));
    }
    const std::uint32_t pe_offset = read_u32_le(bytes, 0x3c);
    if (pe_offset + 6 > static_cast<std::uint32_t>(count)) {
      // The DOS stub may be longer than our 64-byte probe; reopen and seek.
      stream.clear();
      stream.seekg(static_cast<std::streamoff>(pe_offset), std::ios::beg);
      std::array<unsigned char, 6> header{};
      stream.read(reinterpret_cast<char *>(header.data()),
                  static_cast<std::streamsize>(header.size()));
      if (stream.gcount() != static_cast<std::streamsize>(header.size()) ||
          header[0] != 'P' || header[1] != 'E' || header[2] != 0 ||
          header[3] != 0) {
        return info;
      }
      const std::uint16_t machine = static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(header[4]) |
          static_cast<std::uint16_t>(header[5] << 8));
      info.format = BinaryFormat::Pe;
      info.bit_width = 64;
      info.arch = pe_machine_to_arch(machine);
      info.detail = "PE/COFF";
      return info;
    }
    if (bytes[pe_offset] != 'P' || bytes[pe_offset + 1] != 'E' ||
        bytes[pe_offset + 2] != 0 || bytes[pe_offset + 3] != 0) {
      return info;
    }
    info.format = BinaryFormat::Pe;
    info.detail = "PE/COFF";
    info.bit_width = 64;
    info.arch = pe_machine_to_arch(read_u16_le(bytes, pe_offset + 4));
    return info;
  }

  // Mach-O, both endiannesses and 32/64-bit variants.
  const std::uint32_t magic_le = read_u32_le(bytes, 0);
  const std::uint32_t magic_be = read_u32_be(bytes, 0);
  if (magic_le == 0xfeedface || magic_le == 0xfeedfacf ||
      magic_be == 0xfeedface || magic_be == 0xfeedfacf) {
    const bool big_endian = magic_le != 0xfeedface && magic_le != 0xfeedfacf;
    const bool is_64 = (big_endian ? magic_be : magic_le) == 0xfeedfacf;
    const std::uint32_t cpu =
        big_endian ? read_u32_be(bytes, 4) : read_u32_le(bytes, 4);
    info.format = BinaryFormat::MachO;
    info.arch = macho_cpu_to_arch(cpu);
    info.bit_width = is_64 ? 64 : 32;
    info.detail = "Mach-O";
    return info;
  }

  // WebAssembly: 00 61 73 6d.
  if (bytes[0] == 0x00 && bytes[1] == 'a' && bytes[2] == 's' &&
      bytes[3] == 'm') {
    info.format = BinaryFormat::Wasm;
    info.arch = Arch::Wasm32;
    info.bit_width = 32;
    info.detail = "WebAssembly";
    return info;
  }

  // Unix ar archive: "!<arch>\n".
  if (bytes[0] == '!' && bytes[1] == '<' && bytes[2] == 'a' &&
      bytes[3] == 'r' && bytes[4] == 'c' && bytes[5] == 'h' &&
      bytes[6] == '>') {
    info.format = BinaryFormat::Archive;
    info.detail = "ar archive";
    return info;
  }

  return info;
}

bool binary_matches_target(const BinaryInfo &info, const Triple &target) {
  if (target.os == Os::Wasi) {
    return info.format == BinaryFormat::Wasm && info.arch == Arch::Wasm32;
  }
  if (info.format == BinaryFormat::Archive) {
    return true; // archives are handled separately; do not claim a mismatch.
  }
  if (info.arch == Arch::Unknown) {
    return true; // unknown machine: verified structurally, not architecturally.
  }
  if (info.arch != target.arch) {
    return false;
  }
  if (target.os == Os::Windows) {
    return info.format == BinaryFormat::Pe;
  }
  if (target.os == Os::Macos || target.os == Os::Ios) {
    return info.format == BinaryFormat::MachO;
  }
  if (target.os == Os::Linux || target.os == Os::Android ||
      target.os == Os::FreeBsd || target.os == Os::NetBsd ||
      target.os == Os::OpenBsd) {
    return info.format == BinaryFormat::Elf;
  }
  return true;
}

} // namespace cova::platform
