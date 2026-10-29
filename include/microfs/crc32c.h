#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace microfs {
namespace detail {

constexpr std::array<std::uint32_t, 256> MakeCrc32cTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t value = i;
    for (unsigned bit = 0; bit < 8; ++bit) {
      value = (value >> 1U) ^ ((value & 1U) ? 0x82f63b78U : 0U);
    }
    table[i] = value;
  }
  return table;
}

inline constexpr auto kCrc32cTable = MakeCrc32cTable();

}  // namespace detail

inline std::uint32_t Crc32c(std::span<const std::byte> input) {
  std::uint32_t crc = ~0U;
  for (const std::byte byte : input) {
    const auto index =
        static_cast<std::uint8_t>(crc ^ std::to_integer<std::uint8_t>(byte));
    crc = detail::kCrc32cTable[index] ^ (crc >> 8U);
  }
  return ~crc;
}

inline std::uint32_t Crc32c(const char* data, std::size_t size) {
  return Crc32c(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data), size));
}

}  // namespace microfs
