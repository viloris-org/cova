// Cova — content hashing (implementation).
//
// SHA-256 follows FIPS 180-4. The implementation is deliberately plain: Cova
// must be able to verify itself without any external tool.
#include "core/hashing/hash.hpp"

#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace cova {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

void compress(std::uint32_t state[8], const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

constexpr std::uint32_t kInitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

}  // namespace

// ---------------------------------------------------------------------------
// Sha256
// ---------------------------------------------------------------------------

struct Sha256::Impl {
  std::uint32_t state[8];
  std::uint8_t buffer[64];
  std::uint64_t total_bytes = 0;
  std::size_t buffered = 0;

  Impl() { std::memcpy(state, kInitialState, sizeof(state)); }

  void update(const std::uint8_t* data, std::size_t size) {
    total_bytes += static_cast<std::uint64_t>(size);
    if (buffered != 0) {
      const std::size_t take = (size < 64 - buffered) ? size : (64 - buffered);
      std::memcpy(buffer + buffered, data, take);
      buffered += take;
      data += take;
      size -= take;
      if (buffered < 64) {
        return;
      }
      compress(state, buffer);
      buffered = 0;
    }
    while (size >= 64) {
      compress(state, data);
      data += 64;
      size -= 64;
    }
    if (size != 0) {
      std::memcpy(buffer, data, size);
      buffered = size;
    }
  }

  Digest finish() {
    const std::uint64_t bit_length = total_bytes * 8ull;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (buffered != 56) {
      update(&zero, 1);
    }
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
      length_bytes[7 - i] = static_cast<std::uint8_t>((bit_length >> (i * 8)) & 0xffu);
    }
    update(length_bytes, 8);
    Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
      digest[i * 4] = static_cast<std::uint8_t>((state[i] >> 24) & 0xffu);
      digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16) & 0xffu);
      digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8) & 0xffu);
      digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xffu);
    }
    return digest;
  }
};

Sha256::Sha256() : impl_(std::make_unique<Impl>()) {}
Sha256::~Sha256() = default;
Sha256::Sha256(const Sha256& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Sha256& Sha256::operator=(const Sha256& other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>(*other.impl_);
  }
  return *this;
}
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(std::span<const std::byte> data) {
  impl_->update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

void Sha256::update(std::string_view data) {
  impl_->update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

void Sha256::update(const void* data, std::size_t size) {
  impl_->update(static_cast<const std::uint8_t*>(data), size);
}

Digest Sha256::finish() { return impl_->finish(); }

Digest Sha256::of(std::span<const std::byte> data) {
  Sha256 sha;
  sha.update(data);
  return sha.finish();
}

Digest Sha256::of(std::string_view data) {
  Sha256 sha;
  sha.update(data);
  return sha.finish();
}

Result<Digest> Sha256::of_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(Error::make("cova::hash::read", "cannot open file for hashing")
                    .with("path: " + path.string()));
  }
  Sha256 sha;
  std::vector<char> chunk(64 * 1024);
  while (in) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      sha.update(chunk.data(), static_cast<std::size_t>(got));
    }
  }
  return sha.finish();
}

Result<std::string> Sha256::hex_of_file(const std::filesystem::path& path) {
  auto digest = of_file(path);
  if (!digest) {
    return std::unexpected(std::move(digest.error()));
  }
  return to_hex(*digest);
}

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2] = kDigits[(bytes[i] >> 4) & 0x0fu];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0fu];
  }
  return out;
}

std::string to_hex(const Digest& digest) { return to_hex(std::span<const std::uint8_t>(digest)); }

// ---------------------------------------------------------------------------
// Fingerprint
// ---------------------------------------------------------------------------

Fingerprint& Fingerprint::add(std::string_view field) {
  // Length prefix keeps ("ab","c") distinct from ("a","bc").
  const std::uint64_t size = field.size();
  sha_.update(&size, sizeof(size));
  sha_.update(field);
  return *this;
}

Fingerprint& Fingerprint::add(int64_t number) {
  const std::uint64_t size = sizeof(number);
  sha_.update(&size, sizeof(size));
  sha_.update(&number, sizeof(number));
  return *this;
}

Fingerprint& Fingerprint::add_bytes(std::span<const std::byte> data) {
  const std::uint64_t size = data.size();
  sha_.update(&size, sizeof(size));
  sha_.update(data);
  return *this;
}

std::string Fingerprint::finish_hex() { return to_hex(sha_.finish()); }

std::string sha256_hex(std::string_view data) { return to_hex(Sha256::of(data)); }

}  // namespace cova
