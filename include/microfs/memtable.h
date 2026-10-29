#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "microfs/internal_key.h"
#include "microfs/skiplist.h"
#include "microfs/status.h"

namespace microfs {

struct InternalKeyComparator {
  int operator()(std::string_view lhs, std::string_view rhs) const {
    return LexicographicComparator()(lhs, rhs);
  }
};

class MemTable {
 public:
  explicit MemTable(std::size_t arena_block_size = 4096)
      : entries_(InternalKeyComparator(), arena_block_size) {}

  [[nodiscard]] const Status& status() const { return entries_.status(); }

  Status Add(std::uint64_t sequence, ValueType type, std::string_view key,
             std::string_view value = {}) {
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
      std::string internal_key;
      Status status = EncodeInternalKey(key, sequence, type, &internal_key);
      if (!status.ok()) return status;
      status = entries_.Insert(internal_key, value);
      if (!status.ok()) return status;
      approximate_size_.store(entries_.allocated_bytes(),
                              std::memory_order_relaxed);
      last_sequence_.store(sequence, std::memory_order_release);
      return Status::OK();
    } catch (...) {
      return Status::ResourceExhausted("allocating memtable entry");
    }
  }

  Status GetEntry(std::string_view key, std::uint64_t snapshot,
                  std::string* value, std::uint64_t* sequence_out,
                  ValueType* type) const {
    if (value == nullptr || sequence_out == nullptr || type == nullptr) {
      return Status::InvalidArgument("null memtable lookup output");
    }
    if (snapshot > (std::numeric_limits<std::uint64_t>::max() >> 8U)) {
      return Status::InvalidArgument("snapshot sequence is out of range");
    }
    if (snapshot == 0) {
      return Status::NotFound("key not found at sequence zero");
    }
    try {
      std::string lookup;
      Status status =
          EncodeInternalKey(key, snapshot, ValueType::kDeletion, &lookup);
      if (!status.ok()) return status;
      const auto* entry = entries_.FindGreaterOrEqual(lookup);
      if (entry == nullptr) {
        return Status::NotFound("key not found");
      }
      std::string found_key;
      std::uint64_t found_sequence = 0;
      ValueType found_type = ValueType::kDeletion;
      status =
          DecodeInternalKey(entry->key, &found_key, &found_sequence,
                            &found_type);
      if (!status.ok()) return status;
      if (found_key != key || found_sequence > snapshot) {
        return Status::NotFound("key not visible at snapshot");
      }
      value->assign(entry->value);
      *sequence_out = found_sequence;
      *type = found_type;
      return Status::OK();
    } catch (...) {
      return Status::ResourceExhausted("copying memtable entry");
    }
  }

  [[nodiscard]] StatusOr<std::string> Get(std::string_view key,
                                          std::uint64_t snapshot) const {
    std::string value;
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kDeletion;
    Status status = GetEntry(key, snapshot, &value, &sequence, &type);
    if (!status.ok()) return status;
    if (type == ValueType::kDeletion) return Status::NotFound("key deleted");
    return value;
  }

  [[nodiscard]] std::uint64_t last_sequence() const {
    return last_sequence_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::size_t approximate_size() const {
    return approximate_size_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] SkipList<InternalKeyComparator>::Iterator NewIterator() const {
    return entries_.NewIterator();
  }

 private:
  SkipList<InternalKeyComparator> entries_;
  std::atomic<std::uint64_t> last_sequence_{0};
  std::atomic<std::size_t> approximate_size_{0};
};

}  // namespace microfs
