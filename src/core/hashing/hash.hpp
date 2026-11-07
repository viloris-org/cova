// Cova — content hashing (PRD §8.4, §12.3, §15.2, §18.1).
//
// SHA-256 is implemented in-tree: Cova must never depend on an external
// hashing binary for integrity checks.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "core/common.hpp"

namespace cova {

using Digest = std::array<std::uint8_t, 32>;

std::string to_hex(std::span<const std::uint8_t> bytes);
std::string to_hex(const Digest& digest);

class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&);
  Sha256& operator=(const Sha256&);
  Sha256(Sha256&&) noexcept;
  Sha256& operator=(Sha256&&) noexcept;

  void update(std::span<const std::byte> data);
  void update(std::string_view data);
  void update(const void* data, std::size_t size);

  // Finalises the hash. The object must not be reused afterwards.
  Digest finish();

  static Digest of(std::span<const std::byte> data);
  static Digest of(std::string_view data);
  static Result<Digest> of_file(const std::filesystem::path& path);
  static Result<std::string> hex_of_file(const std::filesystem::path& path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Streaming hash builder used to derive stable identifiers from structured
// inputs (cache keys, toolchain fingerprints). Length-prefixes every field so
// that ("ab","c") and ("a","bc") never collide.
class Fingerprint {
 public:
  Fingerprint& add(std::string_view field);
  Fingerprint& add(int64_t number);
  Fingerprint& add_bytes(std::span<const std::byte> data);

  std::string finish_hex();

 private:
  Sha256 sha_;
};

// Convenience: hex SHA-256 of a string.
std::string sha256_hex(std::string_view data);

}  // namespace cova
