#include "microfs/bloom_filter.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

#include "microfs/coding.h"
#include "microfs/hash.h"

namespace microfs {

StatusOr<BloomFilter> BloomFilter::Create(std::size_t expected_entries,
                                          std::uint32_t bits_per_key) {
  if (bits_per_key == 0 || bits_per_key > 1024) {
    return Status::InvalidArgument("bits per key must be between 1 and 1024");
  }
  if (expected_entries >
      (std::numeric_limits<std::size_t>::max() - 7) / bits_per_key) {
    return Status::ResourceExhausted("Bloom filter size overflows");
  }
  const std::size_t requested_bits = expected_entries * bits_per_key;
  const std::size_t bit_count = requested_bits < 64 ? 64 : requested_bits;
  const std::size_t byte_count = (bit_count + 7) / 8;
  if (byte_count > std::numeric_limits<std::size_t>::max() / 8) {
    return Status::ResourceExhausted("Bloom filter allocation overflows");
  }
  const std::uint32_t hashes = std::clamp(
      static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(bits_per_key) * 69 + 50) / 100),
      1U, 30U);
  try {
    return BloomFilter(bits_per_key, static_cast<std::uint8_t>(hashes),
                       std::vector<std::uint8_t>(byte_count));
  } catch (...) {
    return Status::ResourceExhausted("allocating Bloom filter failed");
  }
}

StatusOr<BloomFilter> BloomFilter::Decode(std::string_view encoded) {
  std::uint64_t bits_per_key = 0;
  std::uint64_t hash_count = 0;
  std::string_view bytes;
  if (!GetVarint64(&encoded, &bits_per_key) ||
      !GetVarint64(&encoded, &hash_count) ||
      !GetLengthPrefixed(&encoded, &bytes) || !encoded.empty() ||
      bits_per_key == 0 || bits_per_key > 1024 || hash_count == 0 ||
      hash_count > 30 || bytes.empty()) {
    return Status::Corruption("invalid Bloom filter encoding");
  }
  try {
    std::vector<std::uint8_t> data;
    data.reserve(bytes.size());
    for (const char byte : bytes) {
      data.push_back(static_cast<std::uint8_t>(byte));
    }
    return BloomFilter(static_cast<std::uint32_t>(bits_per_key),
                       static_cast<std::uint8_t>(hash_count), std::move(data));
  } catch (...) {
    return Status::ResourceExhausted("decoding Bloom filter failed");
  }
}

Status BloomFilter::Add(std::string_view key) {
  if (bytes_.empty() || hash_count_ == 0) {
    return Status::InvalidArgument("Bloom filter is not initialized");
  }
  const std::uint32_t hash = MurmurHash3(key);
  const std::uint32_t delta = std::rotl(hash, 15);
  const std::size_t bits = bit_count();
  std::uint32_t probe = hash;
  for (std::uint8_t i = 0; i < hash_count_; ++i) {
    const std::size_t bit = probe % bits;
    bytes_[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
    probe += delta;
  }
  return Status::OK();
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (bytes_.empty() || hash_count_ == 0) return true;
  const std::uint32_t hash = MurmurHash3(key);
  const std::uint32_t delta = std::rotl(hash, 15);
  const std::size_t bits = bit_count();
  std::uint32_t probe = hash;
  for (std::uint8_t i = 0; i < hash_count_; ++i) {
    const std::size_t bit = probe % bits;
    if ((bytes_[bit / 8] & static_cast<std::uint8_t>(1U << (bit % 8))) == 0) {
      return false;
    }
    probe += delta;
  }
  return true;
}

Status BloomFilter::Encode(std::string* output) const {
  if (output == nullptr) return Status::InvalidArgument("null filter output");
  if (bytes_.empty() || hash_count_ == 0) {
    return Status::InvalidArgument("Bloom filter is not initialized");
  }
  try {
    std::string encoded;
    PutVarint64(&encoded, bits_per_key_);
    PutVarint64(&encoded, hash_count_);
    const std::string_view bytes(
        reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
    PutLengthPrefixed(&encoded, bytes);
    *output = std::move(encoded);
  } catch (...) {
    return Status::ResourceExhausted("encoding Bloom filter failed");
  }
  return Status::OK();
}

}  // namespace microfs
