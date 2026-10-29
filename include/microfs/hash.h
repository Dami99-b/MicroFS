#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace microfs {

inline std::uint32_t MurmurHash3(std::string_view input,
                                 std::uint32_t seed = 0) {
  constexpr std::uint32_t kC1 = 0xcc9e2d51U;
  constexpr std::uint32_t kC2 = 0x1b873593U;
  static constexpr std::uint8_t kEmptyInput = 0;
  const auto* data = input.empty()
                         ? &kEmptyInput
                         : reinterpret_cast<const std::uint8_t*>(input.data());
  const std::size_t blocks = input.size() / 4;
  std::uint32_t hash = seed;

  for (std::size_t i = 0; i < blocks; ++i) {
    std::uint32_t key = static_cast<std::uint32_t>(data[4 * i]) |
                        (static_cast<std::uint32_t>(data[4 * i + 1]) << 8U) |
                        (static_cast<std::uint32_t>(data[4 * i + 2]) << 16U) |
                        (static_cast<std::uint32_t>(data[4 * i + 3]) << 24U);
    key *= kC1;
    key = std::rotl(key, 15);
    key *= kC2;
    hash ^= key;
    hash = std::rotl(hash, 13);
    hash = hash * 5U + 0xe6546b64U;
  }

  const auto* tail = data + blocks * 4;
  std::uint32_t key = 0;
  switch (input.size() & 3U) {
    case 3:
      key ^= static_cast<std::uint32_t>(tail[2]) << 16U;
      [[fallthrough]];
    case 2:
      key ^= static_cast<std::uint32_t>(tail[1]) << 8U;
      [[fallthrough]];
    case 1:
      key ^= static_cast<std::uint32_t>(tail[0]);
      key *= kC1;
      key = std::rotl(key, 15);
      key *= kC2;
      hash ^= key;
      break;
    default:
      break;
  }

  hash ^= static_cast<std::uint32_t>(input.size());
  hash ^= hash >> 16U;
  hash *= 0x85ebca6bU;
  hash ^= hash >> 13U;
  hash *= 0xc2b2ae35U;
  hash ^= hash >> 16U;
  return hash;
}

}  // namespace microfs
