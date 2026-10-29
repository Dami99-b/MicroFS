#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace microfs {

inline void PutFixed32(std::string* dst, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    dst->push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

inline void PutFixed64(std::string* dst, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    dst->push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

inline std::uint32_t DecodeFixed32(const char* data) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(data[i]))
             << (8U * i);
  }
  return value;
}

inline std::uint64_t DecodeFixed64(const char* data) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(data[i]))
             << (8U * i);
  }
  return value;
}

inline void PutVarint64(std::string* dst, std::uint64_t value) {
  while (value >= 128) {
    dst->push_back(static_cast<char>((value & 0x7fU) | 0x80U));
    value >>= 7U;
  }
  dst->push_back(static_cast<char>(value));
}

inline bool GetVarint64(std::string_view* input, std::uint64_t* value) {
  std::uint64_t result = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (input->empty()) return false;
    const auto byte = static_cast<std::uint8_t>(input->front());
    input->remove_prefix(1);
    if (shift == 63 && (byte & 0xfeU) != 0) return false;
    result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint64(dst, value.size());
  dst->append(value);
}

inline bool GetLengthPrefixed(std::string_view* input, std::string_view* value) {
  std::uint64_t length = 0;
  if (!GetVarint64(input, &length) || length > input->size()) return false;
  *value = input->substr(0, static_cast<std::size_t>(length));
  input->remove_prefix(static_cast<std::size_t>(length));
  return true;
}

inline void PutBigEndian64(char* output, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    output[i] = static_cast<char>((value >> (56U - 8U * i)) & 0xffU);
  }
}

inline std::uint64_t DecodeBigEndian64(const char* input) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value = (value << 8U) |
            static_cast<std::uint64_t>(
                static_cast<unsigned char>(input[i]));
  }
  return value;
}

}  // namespace microfs
