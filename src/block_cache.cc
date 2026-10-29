#include "microfs/block_cache.h"

#include <algorithm>
#include <limits>

namespace microfs {

BlockCache::BlockCache(std::size_t capacity_bytes, std::size_t shard_count) {
  if (shard_count == 0) shard_count = 1;
  if (capacity_bytes != 0) {
    shard_count = std::min(shard_count, capacity_bytes);
  } else {
    shard_count = 1;
  }
  shards_.reserve(shard_count);
  const std::size_t base_capacity = capacity_bytes / shard_count;
  const std::size_t extra_capacity = capacity_bytes % shard_count;
  for (std::size_t i = 0; i < shard_count; ++i) {
    shards_.push_back(std::make_unique<Shard>(
        base_capacity + static_cast<std::size_t>(i < extra_capacity)));
  }
}

std::size_t BlockCache::KeyHash::operator()(const Key& key) const {
  const std::uint64_t mixed =
      key.table_id ^ (key.block_offset + 0x9e3779b97f4a7c15ULL +
                      (key.table_id << 6U) + (key.table_id >> 2U));
  return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
}

std::size_t BlockCache::ShardIndex(const Key& key) const {
  return KeyHash()(key) % shards_.size();
}

std::shared_ptr<const std::string> BlockCache::Get(
    std::uint64_t table_id, std::uint64_t block_offset) {
  const Key key{table_id, block_offset};
  Shard& shard = *shards_[ShardIndex(key)];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.entries.find(key);
  if (found == shard.entries.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  shard.lru.splice(shard.lru.begin(), shard.lru, found->second);
  hits_.fetch_add(1, std::memory_order_relaxed);
  return found->second->block;
}

Status BlockCache::Insert(std::uint64_t table_id, std::uint64_t block_offset,
                          std::shared_ptr<const std::string> block) {
  if (!block) return Status::InvalidArgument("cannot cache a null block");
  const Key key{table_id, block_offset};
  Shard& shard = *shards_[ShardIndex(key)];
  if (block->size() > shard.capacity || shard.capacity == 0) {
    return Status::OK();
  }
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto existing = shard.entries.find(key);
  if (existing != shard.entries.end()) {
    shard.used -= existing->second->block->size();
    existing->second->block = std::move(block);
    shard.used += existing->second->block->size();
    shard.lru.splice(shard.lru.begin(), shard.lru, existing->second);
  } else {
    try {
      shard.lru.push_front({key, std::move(block)});
      try {
        shard.entries.emplace(key, shard.lru.begin());
      } catch (...) {
        shard.lru.pop_front();
        throw;
      }
    } catch (...) {
      return Status::ResourceExhausted("inserting block cache entry failed");
    }
    shard.used += shard.lru.front().block->size();
  }
  while (shard.used > shard.capacity) {
    const Entry& oldest = shard.lru.back();
    shard.used -= oldest.block->size();
    shard.entries.erase(oldest.key);
    shard.lru.pop_back();
  }
  return Status::OK();
}

double BlockCache::hit_rate() const {
  const std::uint64_t hit_count = hits();
  const std::uint64_t miss_count = misses();
  const std::uint64_t total = hit_count + miss_count;
  if (total == 0) return 0.0;
  return static_cast<double>(hit_count) / static_cast<double>(total);
}

}  // namespace microfs
