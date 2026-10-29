#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "microfs/env.h"
#include "microfs/status.h"

namespace microfs {

enum class SyncPolicy {
  kEveryWrite,
  kEveryNBytes,
  kNever,
};

struct WalOptions {
  SyncPolicy sync_policy = SyncPolicy::kEveryWrite;
  std::size_t sync_bytes = 1024 * 1024;
};

class WAL {
 public:
  static constexpr std::size_t kBlockSize = 32 * 1024;
  static constexpr std::size_t kHeaderSize = 7;

  static StatusOr<std::unique_ptr<WAL>> Open(Env* env, std::string path,
                                             WalOptions options = {});
  static Status Recover(Env* env, const std::string& path,
                        std::vector<std::string>* records,
                        std::uint64_t* truncated_bytes = nullptr);

  WAL(const WAL&) = delete;
  WAL& operator=(const WAL&) = delete;

  Status Append(std::string_view record);
  [[nodiscard]] std::uint64_t fsync_count() const {
    return fsync_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t file_size() const {
    return file_size_.load(std::memory_order_relaxed);
  }

 private:
  struct Request {
    explicit Request(std::string_view bytes) : record(bytes) {}
    std::string record;
    Status status;
    bool done = false;
  };

  WAL(Env* env, std::string path, WalOptions options);
  Status ProcessBatch(const std::vector<Request*>& batch, bool* append_failed);
  Status EncodeRecord(std::string_view record, std::string* output,
                      std::uint64_t* offset) const;

  Env* env_;
  std::string path_;
  WalOptions options_;
  std::uint64_t offset_ = 0;
  std::size_t bytes_since_sync_ = 0;
  std::atomic<std::uint64_t> file_size_{0};
  std::atomic<std::uint64_t> fsync_count_{0};
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<Request*> queue_;
  Status writer_error_;
  bool leader_ = false;
};

}  // namespace microfs
