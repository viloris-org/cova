// Cova — artifact inspection tests (PRD §22.4).
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/binary/binary.hpp"
#include "platform/triple/triple.hpp"
#include "testing/test.hpp"

namespace {

void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

cova::platform::Triple target(const std::string& text) {
  return *cova::platform::Triple::parse(text);
}

} // namespace

COVA_TEST_NAMED(platform, inspects_elf_architecture) {
  cova::test::TempDir temp;
  std::vector<unsigned char> bytes(64, 0);
  bytes[0] = 0x7f;
  bytes[1] = 'E';
  bytes[2] = 'L';
  bytes[3] = 'F';
  bytes[4] = 2;    // 64-bit
  bytes[5] = 1;    // little endian
  bytes[18] = 183; // EM_AARCH64
  const auto path = std::filesystem::path(temp.path()) / "hello";
  write_bytes(path, bytes);

  auto info = COVA_REQUIRE_OK(cova::platform::inspect_binary(path));
  COVA_CHECK(info.format == cova::platform::BinaryFormat::Elf);
  COVA_CHECK(info.arch == cova::platform::Arch::Aarch64);
  COVA_CHECK_EQ(info.bit_width, 64);
  COVA_CHECK(cova::platform::binary_matches_target(info, target("aarch64-unknown-linux-gnu")));
  COVA_CHECK(!cova::platform::binary_matches_target(info, target("x86_64-unknown-linux-gnu")));
}

COVA_TEST_NAMED(platform, inspects_pe_architecture) {
  cova::test::TempDir temp;
  std::vector<unsigned char> bytes(128, 0);
  bytes[0] = 'M';
  bytes[1] = 'Z';
  bytes[0x3c] = 0x40; // e_lfanew
  bytes[0x40] = 'P';
  bytes[0x41] = 'E';
  bytes[0x44] = 0x64; // IMAGE_FILE_MACHINE_ARM64
  bytes[0x45] = 0xaa;
  const auto path = std::filesystem::path(temp.path()) / "hello.exe";
  write_bytes(path, bytes);

  auto info = COVA_REQUIRE_OK(cova::platform::inspect_binary(path));
  COVA_CHECK(info.format == cova::platform::BinaryFormat::Pe);
  COVA_CHECK(info.arch == cova::platform::Arch::Aarch64);
  COVA_CHECK(cova::platform::binary_matches_target(info, target("aarch64-pc-windows-gnu")));
  COVA_CHECK(!cova::platform::binary_matches_target(info, target("aarch64-unknown-linux-gnu")));
}

COVA_TEST_NAMED(platform, inspects_wasm_and_rejects_truncated) {
  cova::test::TempDir temp;
  const auto wasm = std::filesystem::path(temp.path()) / "module.wasm";
  write_bytes(wasm, {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00});
  auto info = COVA_REQUIRE_OK(cova::platform::inspect_binary(wasm));
  COVA_CHECK(info.format == cova::platform::BinaryFormat::Wasm);
  COVA_CHECK(info.arch == cova::platform::Arch::Wasm32);
  COVA_CHECK(cova::platform::binary_matches_target(info, target("wasm32-unknown-wasi")));

  const auto truncated = std::filesystem::path(temp.path()) / "short";
  write_bytes(truncated, {0x7f});
  auto error = COVA_REQUIRE_ERR(cova::platform::inspect_binary(truncated));
  COVA_CHECK(error.code.find("binary") != std::string::npos);
}

COVA_TEST_NAMED(platform, unknown_bytes_are_not_claimed_as_a_target) {
  cova::test::TempDir temp;
  const auto script = std::filesystem::path(temp.path()) / "run.sh";
  write_bytes(script, {'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n'});
  auto info = COVA_REQUIRE_OK(cova::platform::inspect_binary(script));
  COVA_CHECK(info.format == cova::platform::BinaryFormat::Unknown);
  COVA_CHECK(cova::platform::binary_matches_target(info, target("x86_64-unknown-linux-gnu")));
}

COVA_TEST_MAIN()
