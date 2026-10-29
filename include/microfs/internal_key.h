#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "microfs/coding.h"
#include "microfs/status.h"

namespace microfs {

enum class ValueType : std::uint8_t {
  kDeletion = 0,
  kValue = 1,
};

inline Status EncodeInternalKey(std::string_view user_key,
                               std::uint64_t sequence, ValueType type,
                               std::string* encoded) {
  constexpr std::uint64_t kMaxSequence =
      std::numeric_limits<std::uint64_t>::max() >> 8U;
  if (encoded == nullptr || sequence == 0 || sequence > kMaxSequence ||
      (type != ValueType::kDeletion && type != ValueType::kValue)) {
    return Status::InvalidArgument("invalid internal key");
  }
  try {
    encoded->clear();
    if (user_key.size() > encoded->max_size() - 10) {
      return Status::ResourceExhausted("internal key is too large");
    }
    encoded->reserve(user_key.size() + 10);
    for (const char byte : user_key) {
      if (byte == '\0') {
        encoded->push_back('\0');
        encoded->push_back(static_cast<char>(0xff));
      } else {
        encoded->push_back(byte);
      }
    }
    encoded->push_back('\0');
    encoded->push_back('\0');
    const std::uint64_t ordered_sequence =
        (kMaxSequence - sequence) << 8U;
    const std::size_t tag_offset = encoded->size();
    encoded->resize(tag_offset + 8);
    PutBigEndian64(encoded->data() + tag_offset,
                   ordered_sequence | static_cast<std::uint8_t>(type));
    return Status::OK();
  } catch (...) {
    return Status::ResourceExhausted("encoding internal key failed");
  }
}

inline Status DecodeInternalKey(std::string_view encoded,
                                std::string* user_key,
                                std::uint64_t* sequence,
                                ValueType* type) {
  if (user_key == nullptr || sequence == nullptr || type == nullptr) {
    return Status::InvalidArgument("null internal key output");
  }
  if (encoded.size() < 10) return Status::Corruption("short internal key");
  const std::size_t tag_offset = encoded.size() - 8;
  std::size_t cursor = 0;
  try {
    user_key->clear();
    while (cursor < tag_offset) {
      const auto byte = static_cast<std::uint8_t>(encoded[cursor]);
      if (byte != 0) {
        user_key->push_back(encoded[cursor]);
        ++cursor;
        continue;
      }
      if (cursor + 1 >= tag_offset) {
        return Status::Corruption("unterminated internal user key");
      }
      const auto escape = static_cast<std::uint8_t>(encoded[cursor + 1]);
      if (escape == 0) {
        if (cursor + 2 != tag_offset) {
          return Status::Corruption("internal key has invalid terminator");
        }
        cursor += 2;
        break;
      }
      if (escape != 0xffU) {
        return Status::Corruption("invalid internal key escape");
      }
      user_key->push_back('\0');
      cursor += 2;
    }
  } catch (...) {
    return Status::ResourceExhausted("decoding internal key failed");
  }
  if (cursor != tag_offset) {
    return Status::Corruption("internal key has no terminator");
  }
  const std::uint64_t tag = DecodeBigEndian64(encoded.data() + tag_offset);
  const std::uint8_t type_byte = static_cast<std::uint8_t>(tag & 0xffU);
  if (type_byte > static_cast<std::uint8_t>(ValueType::kValue)) {
    return Status::Corruption("invalid internal key value type");
  }
  constexpr std::uint64_t kMaxSequence =
      std::numeric_limits<std::uint64_t>::max() >> 8U;
  *sequence = kMaxSequence - (tag >> 8U);
  if (*sequence == 0) return Status::Corruption("invalid zero sequence");
  *type = static_cast<ValueType>(type_byte);
  return Status::OK();
}

}  // namespace microfs
