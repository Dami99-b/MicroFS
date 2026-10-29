#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "microfs/env.h"
#include "microfs/memtable.h"
#include "microfs/table.h"
#include "microfs/version_set.h"
#include "microfs/wal.h"

namespace microfs {

class WriteBatch {
 public:
  struct Operation {
    ValueType type;
    std::string key;
    std::string value;
  };

  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);
  [[nodiscard]] bool empty() const { return operations_.empty(); }
  [[nodiscard]] const std::vector<Operation>& operations() const {
    return operations_;
  }

 private:
  std::vector<Operation> operations_;
};

struct Snapshot {
  std::uint64_t database_id = 0;
  std::uint64_t id = 0;
  std::uint64_t sequence = 0;
};

struct DBOptions {
  bool create_if_missing = true;
  std::size_t write_buffer_size = 4 * 1024 * 1024;
  std::size_t arena_block_size = 4096;
  std::size_t max_immutable_memtables = 4;
  std::size_t l0_compaction_trigger = 4;
  std::size_t l0_write_slowdown_trigger = 8;
  std::size_t l0_stop_writes_trigger = 12;
  std::size_t block_cache_size = 8 * 1024 * 1024;
  TableOptions table_options;
  WalOptions wal_options;
};

class DBIterator {
 public:
  [[nodiscard]] bool Valid() const;
  [[nodiscard]] std::string_view key() const;
  [[nodiscard]] std::string_view value() const;
  void Seek(std::string_view target);
  void SeekToFirst();
  void SeekToLast();
  void Next();
  void Prev();

 private:
  friend class DB;
  explicit DBIterator(
      std::vector<std::pair<std::string, std::string>> entries);
  std::vector<std::pair<std::string, std::string>> entries_;
  std::size_t position_ = 0;
  bool valid_ = false;
};

class DB {
 public:
  static StatusOr<std::unique_ptr<DB>> Open(Env* env, std::string path,
                                            DBOptions options = {});
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);
  Status Write(const WriteBatch& batch);
  Status Get(std::string_view key, std::string* value,
             const Snapshot* snapshot = nullptr) const;
  StatusOr<Snapshot> GetSnapshot();
  Status ReleaseSnapshot(const Snapshot& snapshot);
  StatusOr<std::unique_ptr<DBIterator>> NewIterator(
      const Snapshot* snapshot = nullptr) const;
  Status Compact();
  Status Close();
  StatusOr<std::string> GetProperty(std::string_view property) const;

 private:
  struct WriteRequest {
    WriteBatch batch;
    Status status;
    bool done = false;
  };

  struct TableState {
    FileMeta metadata;
    std::shared_ptr<Table> table;
  };

  struct LatestValue {
    std::uint64_t sequence;
    ValueType type;
    std::string value;
  };

  struct UserKeyLess {
    bool operator()(const std::string& lhs, const std::string& rhs) const {
      return LexicographicComparator()(lhs, rhs) < 0;
    }
  };

  DB(Env* env, std::string path, DBOptions options);
  Status Initialize();
  Status ApplyWriteGroup(const std::vector<WriteRequest*>& requests);
  Status WaitForWriteSpace();
  Status RotateMemTableLocked();
  void BackgroundLoop();
  Status FlushImmutable(const std::shared_ptr<MemTable>& memtable);
  Status CompactTables();
  Status ValidateSnapshotLocked(const Snapshot* snapshot,
                                std::uint64_t* sequence) const;
  Status CollectMemTableEntries(
      const std::shared_ptr<MemTable>& memtable, std::uint64_t snapshot,
      std::map<std::string, LatestValue, UserKeyLess>* latest) const;
  static Status EncodeWriteGroup(const std::vector<WriteRequest*>& requests,
                                 std::uint64_t first_sequence,
                                 std::string* encoded,
                                 std::uint64_t* last_sequence);

  Env* env_;
  std::string path_;
  DBOptions options_;
  std::uint64_t database_id_ = 0;
  bool locked_ = false;
  std::atomic<bool> closed_{false};
  Status close_status_;
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<WAL> wal_;
  std::unique_ptr<BlockCache> block_cache_;
  std::shared_ptr<MemTable> mutable_memtable_;
  std::vector<std::shared_ptr<MemTable>> immutable_memtables_;
  std::vector<TableState> tables_;
  std::atomic<std::uint64_t> last_sequence_{0};
  std::unordered_map<std::uint64_t, std::uint64_t> snapshots_;
  std::uint64_t next_snapshot_id_ = 1;
  mutable std::mutex state_mutex_;
  std::condition_variable background_condition_;
  std::thread background_thread_;
  bool background_stopping_ = false;
  bool background_busy_ = false;
  bool force_compaction_ = false;
  Status background_error_;
  mutable std::atomic<std::uint64_t> reads_{0};
  std::atomic<std::uint64_t> writes_{0};
  std::atomic<std::uint64_t> compaction_bytes_read_{0};
  std::atomic<std::uint64_t> compaction_bytes_written_{0};
  std::atomic<std::uint64_t> write_stall_micros_{0};
  std::mutex close_mutex_;
  std::mutex write_mutex_;
  std::condition_variable write_condition_;
  std::vector<WriteRequest*> write_queue_;
  bool write_leader_ = false;
  bool closing_ = false;
  Status writer_error_;
};

}  // namespace microfs
