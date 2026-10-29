#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "microfs/coding.h"
#include "microfs/skiplist.h"
#include "microfs/status.h"

namespace microfs {

enum class ValueType : std::uint8_t {
  kDeletion = 0,
  kValue = 1,
};

struct InternalKeyComparator {
  int operator()(std::string_view lhs, std::string_view rhs) const {
    if (lhs.size() < 8 || rhs.size() < 8) {
      return LexicographicComparator()(lhs, rhs);
    }
    const std::string_view lhs_user(lhs.data(), lhs.size() - 8);
    const std::string_view rhs_user(rhs.data(), rhs.size() - 8);
    const int user_order = LexicographicComparator()(lhs_user, rhs_user);
    if (user_order != 0) return user_order;

    const std::uint64_t lhs_tag = DecodeFixed64(lhs.data() + lhs.size() - 8);
    const std::uint64_t rhs_tag = DecodeFixed64(rhs.data() + rhs.size() - 8);
    const std::uint64_t lhs_sequence = lhs_tag >> 8U;
    const std::uint64_t rhs_sequence = rhs_tag >> 8U;
    if (lhs_sequence != rhs_sequence) {
      return lhs_sequence > rhs_sequence ? -1 : 1;
    }
    const auto lhs_type = static_cast<std::uint8_t>(lhs_tag & 0xffU);
    const auto rhs_type = static_cast<std::uint8_t>(rhs_tag & 0xffU);
    if (lhs_type < rhs_type) return -1;
    if (lhs_type > rhs_type) return 1;
    return 0;
  }
};

class MemTable {
 public:
  explicit MemTable(std::size_t arena_block_size = 4096)
      : entries_(InternalKeyComparator(), arena_block_size) {}

  [[nodiscard]] const Status& status() const { return entries_.status(); }

  Status Add(std::uint64_t sequence, ValueType type, std::string_view key,
             std::string_view value = {}) {
    if (sequence == 0 || sequence > (std::numeric_limits<std::uint64_t>::max() >> 8U)) {
      return Status::InvalidArgument("sequence number is out of range");
    }
    if (sequence <= last_sequence_.load(std::memory_order_relaxed)) {
      return Status::InvalidArgument("sequence numbers must increase");
    }
    if (type != ValueType::kValue && type != ValueType::kDeletion) {
      return Status::InvalidArgument("invalid value type");
    }
    if (type == ValueType::kDeletion && !value.empty()) {
      return Status::InvalidArgument("deletion cannot have a value");
    }
    try {
      std::string internal_key(key);
      microfs::PutFixed64(
          &internal_key,
          (sequence << 8U) | static_cast<std::uint8_t>(type));
      Status status = entries_.Insert(internal_key, value);
      if (!status.ok()) return status;
      approximate_size_.store(entries_.allocated_bytes(),
                              std::memory_order_relaxed);
      last_sequence_.store(sequence, std::memory_order_release);
      return Status::OK();
    } catch (...) {
      return Status::ResourceExhausted("allocating memtable entry");
    }
  }

  [[nodiscard]] StatusOr<std::string> Get(std::string_view key,
                                          std::uint64_t snapshot) const {
    if (snapshot > (std::numeric_limits<std::uint64_t>::max() >> 8U)) {
      return Status::InvalidArgument("snapshot sequence is out of range");
    }
    try {
      std::string lookup(key);
      PutFixed64(&lookup, snapshot << 8U);
      const auto* entry = entries_.FindGreaterOrEqual(lookup);
      if (entry == nullptr || entry->key.size() < 8 ||
          entry->key.substr(0, entry->key.size() - 8) != key) {
        return Status::NotFound("key not found");
      }
      const std::uint64_t tag =
          DecodeFixed64(entry->key.data() + entry->key.size() - 8);
      if ((tag >> 8U) > snapshot) {
        return Status::NotFound("key not visible at snapshot");
      }
      if ((tag & 0xffU) == static_cast<std::uint8_t>(ValueType::kDeletion)) {
        return Status::NotFound("key deleted");
      }
      return std::string(entry->value);
    } catch (...) {
      return Status::ResourceExhausted("copying memtable value");
    }
  }

  [[nodiscard]] std::uint64_t last_sequence() const {
    return last_sequence_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::size_t approximate_size() const {
    return approximate_size_.load(std::memory_order_relaxed);
  }

 private:
  SkipList<InternalKeyComparator> entries_;
  std::atomic<std::uint64_t> last_sequence_{0};
  std::atomic<std::size_t> approximate_size_{0};
};

}  // namespace microfs
