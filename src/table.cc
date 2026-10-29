#include "microfs/table.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

#include "microfs/coding.h"
#include "microfs/crc32c.h"
#include "microfs/skiplist.h"

namespace microfs {
namespace {

constexpr std::uint64_t kTableMagic = 0x3153464f5243494dULL;
constexpr std::size_t kFooterSize = 5 * sizeof(std::uint64_t);
constexpr std::uint32_t kFilterVersion = 1;
std::atomic<std::uint64_t> next_table_id{1};

void EncodeHandle(const BlockHandle& handle, std::string* output) {
  PutVarint64(output, handle.offset);
  PutVarint64(output, handle.size);
}

bool DecodeHandle(std::string_view encoded, BlockHandle* handle) {
  return handle != nullptr && GetVarint64(&encoded, &handle->offset) &&
         GetVarint64(&encoded, &handle->size) && encoded.empty();
}

std::uint32_t ReadFixed32(const char* data) {
  return DecodeFixed32(data);
}

std::uint64_t ReadFixed64(const char* data) {
  return DecodeFixed64(data);
}

}  // namespace

TableBuilder::TableBuilder(Env* env, std::string path, TableOptions options)
    : env_(env),
      path_(std::move(path)),
      options_(options),
      data_block_(options.restart_interval),
      index_block_(options.restart_interval) {}

StatusOr<std::unique_ptr<TableBuilder>> TableBuilder::Create(
    Env* env, std::string path, TableOptions options) {
  if (env == nullptr || path.empty() || options.data_block_size == 0 ||
      options.restart_interval == 0 || options.bloom_bits_per_key == 0 ||
      options.prefix_length > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("invalid table builder options");
  }
  try {
    auto builder =
        std::unique_ptr<TableBuilder>(new TableBuilder(env, std::move(path), options));
    Status status = env->WriteFile(builder->path_, {});
    if (!status.ok()) return status;
    return builder;
  } catch (...) {
    return Status::ResourceExhausted("creating table builder failed");
  }
}

Status TableBuilder::Add(std::string_view key, std::string_view value) {
  if (!status_.ok()) return status_;
  if (finished_) return Status::InvalidArgument("table is already finished");
  if (has_last_key_ && LexicographicComparator()(last_key_, key) >= 0) {
    return Status::InvalidArgument("table keys must be strictly increasing");
  }
  Status status = data_block_.Add(key, value);
  if (!status.ok()) return status_ = status;
  try {
    keys_.emplace_back(key);
    if (options_.prefix_length != 0) {
      prefixes_.emplace_back(key.substr(0, options_.prefix_length));
    }
    last_key_.assign(key);
    block_last_key_.assign(key);
  } catch (...) {
    status_ = Status::ResourceExhausted("storing table filter keys failed");
    return status_;
  }
  has_last_key_ = true;
  block_has_entries_ = true;
  if (data_block_.CurrentSizeEstimate() >= options_.data_block_size) {
    return status_ = FlushDataBlock();
  }
  return Status::OK();
}

Status TableBuilder::AppendBlock(std::string_view contents,
                                 BlockHandle* handle) {
  if (handle == nullptr) return Status::InvalidArgument("null block handle");
  if (offset_ > std::numeric_limits<std::uint64_t>::max() -
                    sizeof(std::uint32_t) ||
      contents.size() > std::numeric_limits<std::uint64_t>::max() - offset_ -
                            sizeof(std::uint32_t)) {
    return Status::ResourceExhausted("table file size overflows");
  }
  handle->offset = offset_;
  handle->size = contents.size();
  std::string block;
  try {
    block.assign(contents);
    PutFixed32(&block, Crc32c(contents.data(), contents.size()));
  } catch (...) {
    return Status::ResourceExhausted("assembling table block failed");
  }
  const auto* bytes = reinterpret_cast<const std::byte*>(block.data());
  Status status = env_->AppendFile(path_, {bytes, block.size()});
  if (!status.ok()) return status;
  offset_ += block.size();
  return Status::OK();
}

Status TableBuilder::FlushDataBlock() {
  if (!block_has_entries_) return Status::OK();
  std::string contents;
  Status status = data_block_.Finish(&contents);
  if (!status.ok()) return status;
  BlockHandle handle;
  status = AppendBlock(contents, &handle);
  if (!status.ok()) return status;
  try {
    std::string encoded_handle;
    EncodeHandle(handle, &encoded_handle);
    status = index_block_.Add(block_last_key_, encoded_handle);
  } catch (...) {
    return Status::ResourceExhausted("encoding table index failed");
  }
  if (!status.ok()) return status;
  data_block_.Reset();
  block_last_key_.clear();
  block_has_entries_ = false;
  return Status::OK();
}

Status TableBuilder::WriteFilterBlock(BlockHandle* handle) {
  auto keys = BloomFilter::Create(keys_.size(), options_.bloom_bits_per_key);
  if (!keys.ok()) return keys.status();
  for (const std::string& key : keys_) {
    Status status = keys.value().Add(key);
    if (!status.ok()) return status;
  }

  std::string encoded_keys;
  Status status = keys.value().Encode(&encoded_keys);
  if (!status.ok()) return status;
  std::string encoded_prefixes;
  if (options_.prefix_length != 0) {
    std::sort(prefixes_.begin(), prefixes_.end(),
              [](const std::string& lhs, const std::string& rhs) {
                return LexicographicComparator()(lhs, rhs) < 0;
              });
    prefixes_.erase(std::unique(prefixes_.begin(), prefixes_.end()),
                    prefixes_.end());
    auto prefixes =
        BloomFilter::Create(prefixes_.size(), options_.bloom_bits_per_key);
    if (!prefixes.ok()) return prefixes.status();
    for (const std::string& prefix : prefixes_) {
      status = prefixes.value().Add(prefix);
      if (!status.ok()) return status;
    }
    status = prefixes.value().Encode(&encoded_prefixes);
    if (!status.ok()) return status;
  }

  std::string filter_block;
  try {
    PutFixed32(&filter_block, kFilterVersion);
    PutFixed32(&filter_block,
               static_cast<std::uint32_t>(options_.prefix_length));
    PutLengthPrefixed(&filter_block, encoded_keys);
    PutLengthPrefixed(&filter_block, encoded_prefixes);
  } catch (...) {
    return Status::ResourceExhausted("building table filter block failed");
  }
  return AppendBlock(filter_block, handle);
}

Status TableBuilder::Finish() {
  if (!status_.ok()) return status_;
  if (finished_) return Status::InvalidArgument("table is already finished");
  if (block_has_entries_) {
    status_ = FlushDataBlock();
    if (!status_.ok()) return status_;
  }

  std::string index_contents;
  status_ = index_block_.Finish(&index_contents);
  if (!status_.ok()) return status_;
  BlockHandle index_handle;
  status_ = AppendBlock(index_contents, &index_handle);
  if (!status_.ok()) return status_;

  BlockHandle filter_handle;
  status_ = WriteFilterBlock(&filter_handle);
  if (!status_.ok()) return status_;

  std::string footer;
  try {
    PutFixed64(&footer, kTableMagic);
    PutFixed64(&footer, index_handle.offset);
    PutFixed64(&footer, index_handle.size);
    PutFixed64(&footer, filter_handle.offset);
    PutFixed64(&footer, filter_handle.size);
  } catch (...) {
    return status_ =
               Status::ResourceExhausted("building table footer failed");
  }
  const auto* bytes = reinterpret_cast<const std::byte*>(footer.data());
  status_ = env_->AppendFile(path_, {bytes, footer.size()});
  if (!status_.ok()) return status_;
  offset_ += footer.size();
  status_ = env_->SyncFile(path_);
  if (!status_.ok()) return status_;
  finished_ = true;
  return Status::OK();
}

Table::Table(std::string path, std::string file_data, BlockCache* cache)
    : path_(std::move(path)),
      file_data_(std::move(file_data)),
      cache_(cache),
      cache_id_(next_table_id.fetch_add(1, std::memory_order_relaxed)) {}

StatusOr<std::unique_ptr<Table>> Table::Open(Env* env, std::string path,
                                             BlockCache* cache) {
  if (env == nullptr || path.empty()) {
    return Status::InvalidArgument("table requires an Env and a path");
  }
  try {
    std::vector<std::byte> file;
    Status status = env->ReadFile(path, &file);
    if (!status.ok()) return status;
    if (file.empty()) return Status::Corruption("table file is empty");
    std::string contents(reinterpret_cast<const char*>(file.data()), file.size());
    auto table = std::unique_ptr<Table>(
        new Table(std::move(path), std::move(contents), cache));
    status = table->Initialize();
    if (!status.ok()) return status;
    return table;
  } catch (...) {
    return Status::ResourceExhausted("opening table failed");
  }
}

Status Table::Initialize() {
  if (file_data_.size() < kFooterSize) {
    return Status::Corruption("table file is shorter than its footer");
  }
  footer_offset_ = file_data_.size() - kFooterSize;
  const char* footer = file_data_.data() + footer_offset_;
  if (ReadFixed64(footer) != kTableMagic) {
    return Status::Corruption("table footer magic does not match");
  }
  index_handle_ = {ReadFixed64(footer + 8), ReadFixed64(footer + 16)};
  filter_handle_ = {ReadFixed64(footer + 24), ReadFixed64(footer + 32)};
  if (index_handle_.offset > footer_offset_ ||
      index_handle_.size > footer_offset_ - index_handle_.offset ||
      sizeof(std::uint32_t) >
          footer_offset_ - index_handle_.offset - index_handle_.size) {
    return Status::Corruption("table index handle is out of bounds");
  }
  const std::uint64_t index_end =
      index_handle_.offset + index_handle_.size + sizeof(std::uint32_t);
  if (filter_handle_.offset < index_end ||
      filter_handle_.offset > footer_offset_ ||
      filter_handle_.size > footer_offset_ - filter_handle_.offset ||
      sizeof(std::uint32_t) !=
          footer_offset_ - filter_handle_.offset - filter_handle_.size) {
    return Status::Corruption("table filter handle is out of bounds");
  }
  Status status = ReadBlock(index_handle_, &index_data_);
  if (!status.ok()) return status;
  BlockReader index_reader;
  status = index_reader.Open(index_data_);
  if (!status.ok()) return status;
  index_reader.SeekToFirst();
  std::uint64_t expected_data_offset = 0;
  std::string previous_last_key;
  bool has_previous_last_key = false;
  while (index_reader.Valid()) {
    BlockHandle data_handle;
    if (!DecodeHandle(index_reader.value(), &data_handle) ||
        data_handle.offset != expected_data_offset ||
        data_handle.offset > index_handle_.offset ||
        data_handle.size >
            index_handle_.offset - data_handle.offset ||
        sizeof(std::uint32_t) >
            index_handle_.offset - data_handle.offset - data_handle.size) {
      return Status::Corruption("invalid data block handle in table index");
    }
    std::string data_contents;
    status = ReadBlock(data_handle, &data_contents);
    if (!status.ok()) return status;
    BlockReader data_reader;
    status = data_reader.Open(data_contents);
    if (!status.ok()) return status;
    data_reader.SeekToFirst();
    if (!data_reader.Valid()) {
      return Status::Corruption("table index references an empty data block");
    }
    if (has_previous_last_key &&
        LexicographicComparator()(previous_last_key, data_reader.key()) >= 0) {
      return Status::Corruption("data blocks are not ordered: " +
                                previous_last_key + " >= " +
                                std::string(data_reader.key()));
    }
    data_reader.SeekToLast();
    if (!data_reader.Valid() || data_reader.key() != index_reader.key()) {
      return Status::Corruption("table index key does not match data block");
    }
    previous_last_key.assign(data_reader.key());
    has_previous_last_key = true;
    expected_data_offset += data_handle.size + sizeof(std::uint32_t);
    index_reader.Next();
  }
  if (expected_data_offset != index_handle_.offset) {
    return Status::Corruption("table index does not cover all data blocks");
  }
  std::string filter_contents;
  status = ReadBlock(filter_handle_, &filter_contents);
  if (!status.ok()) return status;
  return DecodeFilterBlock(filter_contents);
}

Status Table::ReadBlock(const BlockHandle& handle,
                        std::string* contents) const {
  if (contents == nullptr || handle.offset > file_data_.size() ||
      handle.size > file_data_.size() - handle.offset ||
      sizeof(std::uint32_t) >
          file_data_.size() - handle.offset - handle.size) {
    return Status::Corruption("table block handle is out of bounds");
  }
  const char* block = file_data_.data() + handle.offset;
  const auto size = static_cast<std::size_t>(handle.size);
  const std::uint32_t expected_crc = DecodeFixed32(block + size);
  if (Crc32c(block, size) != expected_crc) {
    return Status::Corruption("table block checksum mismatch");
  }
  try {
    contents->assign(block, size);
  } catch (...) {
    return Status::ResourceExhausted("reading table block failed");
  }
  return Status::OK();
}

Status Table::DecodeFilterBlock(std::string_view encoded) {
  if (encoded.size() < 8 ||
      ReadFixed32(encoded.data()) != kFilterVersion) {
    return Status::Corruption("invalid table filter version");
  }
  prefix_length_ = ReadFixed32(encoded.data() + 4);
  encoded.remove_prefix(8);
  std::string_view key_encoded;
  std::string_view prefix_encoded;
  if (!GetLengthPrefixed(&encoded, &key_encoded) ||
      !GetLengthPrefixed(&encoded, &prefix_encoded) || !encoded.empty()) {
    return Status::Corruption("malformed table filter block");
  }
  auto key_filter = BloomFilter::Decode(key_encoded);
  if (!key_filter.ok()) return key_filter.status();
  key_filter_ = std::move(key_filter).value();
  if (prefix_length_ != 0) {
    if (prefix_encoded.empty()) {
      return Status::Corruption("missing prefix Bloom filter");
    }
    auto prefix_filter = BloomFilter::Decode(prefix_encoded);
    if (!prefix_filter.ok()) return prefix_filter.status();
    prefix_filter_ = std::move(prefix_filter).value();
  } else if (!prefix_encoded.empty()) {
    return Status::Corruption("unexpected prefix Bloom filter");
  }
  return Status::OK();
}

Status Table::LoadDataBlock(
    const BlockHandle& handle,
    std::shared_ptr<const std::string>* contents) const {
  if (contents == nullptr) {
    return Status::InvalidArgument("null table block output");
  }
  if (cache_ != nullptr) {
    auto cached = cache_->Get(cache_id_, handle.offset);
    if (cached) {
      *contents = std::move(cached);
      return Status::OK();
    }
  }
  std::string data;
  Status status = ReadBlock(handle, &data);
  if (!status.ok()) return status;
  try {
    auto block = std::make_shared<const std::string>(std::move(data));
    if (cache_ != nullptr) {
      status = cache_->Insert(cache_id_, handle.offset, block);
      if (!status.ok()) return status;
    }
    *contents = std::move(block);
  } catch (...) {
    return Status::ResourceExhausted("caching table block failed");
  }
  return Status::OK();
}

Status Table::Get(std::string_view key, std::string* value) const {
  if (value == nullptr) return Status::InvalidArgument("null table value output");
  if (!key_filter_.MayContain(key) || !MayContainPrefix(key)) {
    return Status::NotFound("key was excluded by table filter");
  }
  BlockReader index;
  Status status = index.Open(index_data_);
  if (!status.ok()) return status;
  index.Seek(key);
  if (!index.status().ok()) return index.status();
  if (!index.Valid()) return Status::NotFound("key is not in this table");
  BlockHandle handle;
  if (!DecodeHandle(index.value(), &handle)) {
    return Status::Corruption("invalid table index entry");
  }
  std::shared_ptr<const std::string> data;
  status = LoadDataBlock(handle, &data);
  if (!status.ok()) return status;
  BlockReader block;
  status = block.Open(*data);
  if (!status.ok()) return status;
  block.Seek(key);
  if (!block.status().ok()) return block.status();
  if (!block.Valid() || block.key() != key) {
    return Status::NotFound("key is not in this table");
  }
  try {
    value->assign(block.value());
  } catch (...) {
    return Status::ResourceExhausted("copying table value failed");
  }
  return Status::OK();
}

bool Table::MayContain(std::string_view key) const {
  return key_filter_.MayContain(key);
}

bool Table::MayContainPrefix(std::string_view key) const {
  if (prefix_length_ == 0) return true;
  const std::string_view prefix = key.substr(0, prefix_length_);
  return prefix_filter_.MayContain(prefix);
}

}  // namespace microfs
