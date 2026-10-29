#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "microfs/block.h"
#include "microfs/block_cache.h"
#include "microfs/bloom_filter.h"
#include "microfs/env.h"
#include "microfs/status.h"

namespace microfs {

struct TableOptions {
  std::size_t data_block_size = 4096;
  std::size_t restart_interval = 16;
  std::uint32_t bloom_bits_per_key = 10;
  std::size_t prefix_length = 0;
};

struct BlockHandle {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

class TableBuilder {
 public:
  static StatusOr<std::unique_ptr<TableBuilder>> Create(
      Env* env, std::string path, TableOptions options = {});
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  Status Add(std::string_view key, std::string_view value);
  Status Finish();
  [[nodiscard]] std::uint64_t file_size() const { return offset_; }

 private:
  TableBuilder(Env* env, std::string path, TableOptions options);
  Status FlushDataBlock();
  Status AppendBlock(std::string_view contents, BlockHandle* handle);
  Status WriteFilterBlock(BlockHandle* handle);

  Env* env_;
  std::string path_;
  TableOptions options_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  std::string block_last_key_;
  bool has_last_key_ = false;
  bool block_has_entries_ = false;
  bool finished_ = false;
  std::uint64_t offset_ = 0;
  Status status_;
  std::vector<std::string> keys_;
  std::vector<std::string> prefixes_;
};

class Table {
 public:
  static StatusOr<std::unique_ptr<Table>> Open(
      Env* env, std::string path, BlockCache* cache = nullptr);
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  Status Get(std::string_view key, std::string* value) const;
  Status GetAtOrAfter(std::string_view key, std::string* found_key,
                      std::string* value) const;
  Status ReadEntries(
      std::vector<std::pair<std::string, std::string>>* entries) const;
  [[nodiscard]] bool MayContain(std::string_view key) const;
  [[nodiscard]] bool MayContainPrefix(std::string_view key) const;
  [[nodiscard]] std::uint64_t file_size() const { return file_data_.size(); }

 private:
  Table(std::string path, std::string file_data, BlockCache* cache);
  Status Initialize();
  Status ReadBlock(const BlockHandle& handle, std::string* contents) const;
  Status LoadDataBlock(const BlockHandle& handle,
                       std::shared_ptr<const std::string>* contents) const;
  Status DecodeFilterBlock(std::string_view encoded);

  std::string path_;
  std::string file_data_;
  BlockCache* cache_;
  std::uint64_t cache_id_ = 0;
  std::uint64_t footer_offset_ = 0;
  BlockHandle index_handle_;
  BlockHandle filter_handle_;
  std::string index_data_;
  BloomFilter key_filter_;
  BloomFilter prefix_filter_;
  std::size_t prefix_length_ = 0;
};

}  // namespace microfs
