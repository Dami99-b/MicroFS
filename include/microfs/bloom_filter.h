#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "microfs/status.h"

namespace microfs {

class BloomFilter {
 public:
  BloomFilter() = default;

  static StatusOr<BloomFilter> Create(std::size_t expected_entries,
                                      std::uint32_t bits_per_key = 10);
  static StatusOr<BloomFilter> Decode(std::string_view encoded);

  Status Add(std::string_view key);
  [[nodiscard]] bool MayContain(std::string_view key) const;
  Status Encode(std::string* output) const;
  [[nodiscard]] std::uint32_t bits_per_key() const { return bits_per_key_; }
  [[nodiscard]] std::size_t bit_count() const { return bytes_.size() * 8; }
  [[nodiscard]] std::size_t byte_size() const { return bytes_.size(); }

 private:
  BloomFilter(std::uint32_t bits_per_key, std::uint8_t hash_count,
              std::vector<std::uint8_t> bytes)
      : bits_per_key_(bits_per_key),
        hash_count_(hash_count),
        bytes_(std::move(bytes)) {}

  std::uint32_t bits_per_key_ = 10;
  std::uint8_t hash_count_ = 7;
  std::vector<std::uint8_t> bytes_;
};

}  // namespace microfs
