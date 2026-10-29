#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "microfs/status.h"

namespace microfs {

class BlockCache {
 public:
  explicit BlockCache(std::size_t capacity_bytes,
                      std::size_t shard_count = 16);
  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  [[nodiscard]] std::shared_ptr<const std::string> Get(
      std::uint64_t table_id, std::uint64_t block_offset);
  Status Insert(std::uint64_t table_id, std::uint64_t block_offset,
                std::shared_ptr<const std::string> block);

  [[nodiscard]] std::uint64_t hits() const {
    return hits_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t misses() const {
    return misses_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] double hit_rate() const;

 private:
  struct Key {
    std::uint64_t table_id;
    std::uint64_t block_offset;
    bool operator==(const Key&) const = default;
  };

  struct KeyHash {
    std::size_t operator()(const Key& key) const;
  };

  struct Entry {
    Key key;
    std::shared_ptr<const std::string> block;
  };

  struct Shard {
    explicit Shard(std::size_t capacity) : capacity(capacity) {}
    std::mutex mutex;
    std::size_t capacity;
    std::size_t used = 0;
    std::list<Entry> lru;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> entries;
  };

  std::size_t ShardIndex(const Key& key) const;

  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> hits_{0};
  std::atomic<std::uint64_t> misses_{0};
};

}  // namespace microfs
