#include "microfs/wal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#include "microfs/coding.h"
#include "microfs/crc32c.h"

namespace microfs {
namespace {

constexpr std::uint8_t kFullType = 1;
constexpr std::uint8_t kFirstType = 2;
constexpr std::uint8_t kMiddleType = 3;
constexpr std::uint8_t kLastType = 4;

std::uint16_t DecodeFixed16(const char* data) {
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(data[0]) |
      (static_cast<std::uint16_t>(static_cast<unsigned char>(data[1])) << 8U));
}

void PutFixed16(char* output, std::uint16_t value) {
  output[0] = static_cast<char>(value & 0xffU);
  output[1] = static_cast<char>(value >> 8U);
}

bool AllZero(const std::byte* data, std::size_t size) {
  return std::all_of(data, data + size,
                     [](std::byte value) { return value == std::byte{0}; });
}

}  // namespace

WAL::WAL(Env* env, std::string path, WalOptions options)
    : env_(env), path_(std::move(path)), options_(options) {}

StatusOr<std::unique_ptr<WAL>> WAL::Open(Env* env, std::string path,
                                         WalOptions options) {
  if (env == nullptr || path.empty()) {
    return Status::InvalidArgument("WAL requires an Env and a path");
  }
  if (options.sync_policy == SyncPolicy::kEveryNBytes &&
      options.sync_bytes == 0) {
    return Status::InvalidArgument("sync_bytes must be nonzero");
  }
  try {
    std::vector<std::string> recovered;
    std::uint64_t truncated = 0;
    Status status = Recover(env, path, &recovered, &truncated);
    if (status.code() == StatusCode::kNotFound) {
      status = env->WriteFile(path, {});
      if (!status.ok()) return status;
      status = Status::OK();
    }
    if (!status.ok()) return status;
    std::vector<std::byte> file;
    status = env->ReadFile(path, &file);
    if (!status.ok()) return status;
    auto wal = std::unique_ptr<WAL>(new WAL(env, std::move(path), options));
    wal->offset_ = file.size();
    wal->file_size_.store(file.size(), std::memory_order_relaxed);
    return wal;
  } catch (...) {
    return Status::ResourceExhausted("opening WAL exhausted memory");
  }
}

Status WAL::Recover(Env* env, const std::string& path,
                    std::vector<std::string>* records,
                    std::uint64_t* truncated_bytes) {
  if (env == nullptr || path.empty() || records == nullptr) {
    return Status::InvalidArgument("invalid WAL recovery arguments");
  }
  records->clear();
  if (truncated_bytes != nullptr) *truncated_bytes = 0;
  std::vector<std::byte> file;
  Status status = env->ReadFile(path, &file);
  if (!status.ok()) return status;

  const std::uint64_t file_size = file.size();
  std::uint64_t offset = 0;
  std::uint64_t last_good_offset = 0;
  std::uint64_t fragmented_record_start = 0;
  bool assembling = false;
  std::string fragmented;

  try {
    while (offset < file_size) {
      const std::size_t block_offset =
          static_cast<std::size_t>(offset % kBlockSize);
      const std::size_t block_remaining = kBlockSize - block_offset;
      const std::size_t available = static_cast<std::size_t>(
          std::min<std::uint64_t>(block_remaining, file_size - offset));
      const auto* data = file.data() + static_cast<std::size_t>(offset);

      if (available < kHeaderSize) {
        if (!AllZero(data, available)) break;
        offset += available;
        if (offset == file_size) break;
        continue;
      }

      const auto* header = reinterpret_cast<const char*>(data);
      const std::uint32_t expected_crc = DecodeFixed32(header);
      const std::uint16_t length = DecodeFixed16(header + 4);
      const auto type = static_cast<std::uint8_t>(header[6]);

      if (expected_crc == 0 && length == 0 && type == 0) {
        if (!AllZero(data, available)) break;
        offset += available;
        if (available < block_remaining) break;
        continue;
      }
      if (length > block_remaining - kHeaderSize ||
          static_cast<std::uint64_t>(kHeaderSize) + length > file_size - offset) {
        break;
      }

      const std::uint64_t record_offset = offset;
      const char* payload = header + kHeaderSize;
      std::string checksum_input;
      checksum_input.reserve(static_cast<std::size_t>(length) + 1);
      checksum_input.push_back(static_cast<char>(type));
      checksum_input.append(payload, length);
      if (Crc32c(checksum_input.data(), checksum_input.size()) != expected_crc) {
        break;
      }

      const std::uint64_t record_end = offset + kHeaderSize + length;
      bool valid_fragment = true;
      switch (type) {
        case kFullType:
          if (assembling) {
            valid_fragment = false;
          } else {
            records->emplace_back(payload, length);
            last_good_offset = record_end;
          }
          break;
        case kFirstType:
          if (assembling) {
            valid_fragment = false;
          } else {
            fragmented_record_start = record_offset;
            fragmented.assign(payload, length);
            assembling = true;
          }
          break;
        case kMiddleType:
          if (!assembling) {
            valid_fragment = false;
          } else {
            fragmented.append(payload, length);
          }
          break;
        case kLastType:
          if (!assembling) {
            valid_fragment = false;
          } else {
            fragmented.append(payload, length);
            records->push_back(std::move(fragmented));
            fragmented.clear();
            assembling = false;
            last_good_offset = record_end;
          }
          break;
        default:
          valid_fragment = false;
          break;
      }
      if (!valid_fragment) break;
      offset = record_end;
    }
  } catch (...) {
    return Status::ResourceExhausted("recovering WAL exhausted memory");
  }

  if (assembling) last_good_offset = fragmented_record_start;
  if (last_good_offset < file_size) {
    status = env->TruncateFile(path, last_good_offset);
    if (!status.ok()) return status;
    status = env->SyncFile(path);
    if (!status.ok()) return status;
    if (truncated_bytes != nullptr) {
      *truncated_bytes = file_size - last_good_offset;
    }
  }
  return Status::OK();
}

Status WAL::Append(std::string_view record) {
  Request request({});
  try {
    request.record.assign(record);
  } catch (...) {
    return Status::ResourceExhausted("copying WAL record exhausted memory");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (!writer_error_.ok()) return writer_error_;
  try {
    queue_.push_back(&request);
  } catch (...) {
    return Status::ResourceExhausted("queueing WAL record exhausted memory");
  }
  if (leader_) {
    condition_.wait(lock, [&request] { return request.done; });
    return request.status;
  }

  leader_ = true;
  while (true) {
    std::vector<Request*> batch;
    batch.swap(queue_);
    Status status = writer_error_;
    if (status.ok()) {
      bool append_failed = false;
      lock.unlock();
      status = ProcessBatch(batch, &append_failed);
      lock.lock();
      if (append_failed) writer_error_ = status;
    }
    for (Request* queued : batch) {
      queued->status = writer_error_.ok() ? status : writer_error_;
      queued->done = true;
    }
    condition_.notify_all();
    if (queue_.empty()) {
      leader_ = false;
      condition_.notify_all();
      return request.status;
    }
  }
}

Status WAL::EncodeRecord(std::string_view record, std::string* output,
                         std::uint64_t* offset) const {
  if (output == nullptr || offset == nullptr) {
    return Status::InvalidArgument("null WAL encoding output");
  }
  std::size_t position = 0;
  bool begin = true;
  const bool empty = record.empty();
  do {
    const std::size_t block_offset =
        static_cast<std::size_t>(*offset % kBlockSize);
    const std::size_t block_remaining = kBlockSize - block_offset;
    if (block_remaining < kHeaderSize ||
        (!empty && block_remaining == kHeaderSize)) {
      output->append(block_remaining, '\0');
      *offset += block_remaining;
      continue;
    }
    const std::size_t available = block_remaining - kHeaderSize;
    const std::size_t length =
        empty ? 0 : std::min(record.size() - position, available);
    const bool end = empty || position + length == record.size();
    const std::uint8_t type =
        begin ? (end ? kFullType : kFirstType)
              : (end ? kLastType : kMiddleType);
    std::string checksum_input;
    checksum_input.reserve(length + 1);
    checksum_input.push_back(static_cast<char>(type));
    if (length != 0) checksum_input.append(record.data() + position, length);
    const std::uint32_t crc =
        Crc32c(checksum_input.data(), checksum_input.size());
    const std::size_t start = output->size();
    output->resize(start + kHeaderSize);
    for (unsigned i = 0; i < 4; ++i) {
      (*output)[start + i] = static_cast<char>((crc >> (8U * i)) & 0xffU);
    }
    PutFixed16(output->data() + start + 4, static_cast<std::uint16_t>(length));
    (*output)[start + 6] = static_cast<char>(type);
    if (length != 0) output->append(record.data() + position, length);
    *offset += kHeaderSize + length;
    position += length;
    begin = false;
    if (end) break;
  } while (true);
  return Status::OK();
}

Status WAL::ProcessBatch(const std::vector<Request*>& batch,
                         bool* append_failed) {
  if (append_failed == nullptr) {
    return Status::InvalidArgument("null WAL append status output");
  }
  *append_failed = false;
  std::string output;
  std::uint64_t next_offset = offset_;
  try {
    for (const Request* request : batch) {
      Status status = EncodeRecord(request->record, &output, &next_offset);
      if (!status.ok()) return status;
    }
  } catch (...) {
    return Status::ResourceExhausted("encoding WAL batch exhausted memory");
  }
  if (output.empty()) return Status::OK();
  const auto* bytes = reinterpret_cast<const std::byte*>(output.data());
  Status status = env_->AppendFile(path_, {bytes, output.size()});
  if (!status.ok()) {
    *append_failed = true;
    return status;
  }
  offset_ = next_offset;
  file_size_.store(offset_, std::memory_order_relaxed);
  if (output.size() >
      std::numeric_limits<std::size_t>::max() - bytes_since_sync_) {
    bytes_since_sync_ = std::numeric_limits<std::size_t>::max();
  } else {
    bytes_since_sync_ += output.size();
  }

  const bool should_sync =
      options_.sync_policy == SyncPolicy::kEveryWrite ||
      (options_.sync_policy == SyncPolicy::kEveryNBytes &&
       output.size() >= options_.sync_bytes -
                            std::min(bytes_since_sync_, options_.sync_bytes));
  if (should_sync) {
    status = env_->SyncFile(path_);
    if (!status.ok()) return status;
    fsync_count_.fetch_add(1, std::memory_order_relaxed);
    bytes_since_sync_ = 0;
  }
  return Status::OK();
}

}  // namespace microfs
