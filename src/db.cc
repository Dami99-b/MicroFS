#include "microfs/db.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <new>
#include <utility>

#include "microfs/coding.h"
#include "microfs/internal_key.h"

namespace microfs {
namespace {

constexpr std::uint32_t kWriteBatchFormat = 1;
std::atomic<std::uint64_t> next_database_id{1};

struct LoggedOperation {
  std::uint64_t sequence;
  ValueType type;
  std::string key;
  std::string value;
};

bool ReadFixed64(std::string_view* encoded, std::uint64_t* value) {
  if (encoded->size() < 8) return false;
  *value = DecodeFixed64(encoded->data());
  encoded->remove_prefix(8);
  return true;
}

std::string JoinPath(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

Status DecodeWriteGroup(std::string_view encoded,
                        std::vector<LoggedOperation>* operations) {
  if (operations == nullptr || encoded.size() < 4) {
    return Status::Corruption("short write batch record");
  }
  const std::uint32_t format = DecodeFixed32(encoded.data());
  encoded.remove_prefix(4);
  std::uint64_t count = 0;
  if (format != kWriteBatchFormat || !GetVarint64(&encoded, &count) ||
      count == 0 || count > encoded.size() / 10) {
    return Status::Corruption("invalid write batch header");
  }
  try {
    operations->clear();
    operations->reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      LoggedOperation operation{};
      if (!ReadFixed64(&encoded, &operation.sequence) || encoded.empty()) {
        return Status::Corruption("truncated write batch operation");
      }
      const auto type = static_cast<std::uint8_t>(encoded.front());
      encoded.remove_prefix(1);
      std::string_view key;
      if (type > static_cast<std::uint8_t>(ValueType::kValue) ||
          !GetLengthPrefixed(&encoded, &key)) {
        return Status::Corruption("invalid write batch key or type");
      }
      operation.type = static_cast<ValueType>(type);
      operation.key.assign(key);
      if (operation.type == ValueType::kValue) {
        std::string_view value;
        if (!GetLengthPrefixed(&encoded, &value)) {
          return Status::Corruption("truncated write batch value");
        }
        operation.value.assign(value);
      }
      operations->push_back(std::move(operation));
    }
  } catch (...) {
    return Status::ResourceExhausted("decoding write batch failed");
  }
  if (!encoded.empty()) return Status::Corruption("write batch has trailing bytes");
  return Status::OK();
}

}  // namespace

Status WriteBatch::Put(std::string_view key, std::string_view value) {
  try {
    operations_.push_back(
        {ValueType::kValue, std::string(key), std::string(value)});
  } catch (...) {
    return Status::ResourceExhausted("adding write batch value failed");
  }
  return Status::OK();
}

Status WriteBatch::Delete(std::string_view key) {
  try {
    operations_.push_back({ValueType::kDeletion, std::string(key), {}});
  } catch (...) {
    return Status::ResourceExhausted("adding write batch deletion failed");
  }
  return Status::OK();
}

DBIterator::DBIterator(
    std::vector<std::pair<std::string, std::string>> entries)
    : entries_(std::move(entries)) {}

bool DBIterator::Valid() const {
  return valid_ && position_ < entries_.size();
}

std::string_view DBIterator::key() const {
  return Valid() ? std::string_view(entries_[position_].first)
                 : std::string_view();
}

std::string_view DBIterator::value() const {
  return Valid() ? std::string_view(entries_[position_].second)
                 : std::string_view();
}

void DBIterator::Seek(std::string_view target) {
  const auto found = std::lower_bound(
      entries_.begin(), entries_.end(), target,
      [](const auto& entry, std::string_view key) {
        return LexicographicComparator()(entry.first, key) < 0;
      });
  position_ = static_cast<std::size_t>(found - entries_.begin());
  valid_ = position_ < entries_.size();
}

void DBIterator::SeekToFirst() {
  position_ = 0;
  valid_ = !entries_.empty();
}

void DBIterator::SeekToLast() {
  valid_ = !entries_.empty();
  position_ = valid_ ? entries_.size() - 1 : 0;
}

void DBIterator::Next() {
  if (!Valid()) return;
  ++position_;
  valid_ = position_ < entries_.size();
}

void DBIterator::Prev() {
  if (!Valid()) return;
  if (position_ == 0) {
    valid_ = false;
    return;
  }
  --position_;
}

DB::DB(Env* env, std::string path, DBOptions options)
    : env_(env),
      path_(std::move(path)),
      options_(options),
      database_id_(next_database_id.fetch_add(1, std::memory_order_relaxed)) {}

StatusOr<std::unique_ptr<DB>> DB::Open(Env* env, std::string path,
                                       DBOptions options) {
  if (env == nullptr || path.empty() || options.write_buffer_size == 0 ||
      options.arena_block_size == 0 || options.max_immutable_memtables == 0 ||
      options.l0_compaction_trigger == 0 ||
      options.l0_write_slowdown_trigger < options.l0_compaction_trigger ||
      options.l0_stop_writes_trigger < options.l0_write_slowdown_trigger) {
    return Status::InvalidArgument("invalid database options");
  }
  try {
    auto db = std::unique_ptr<DB>(new DB(env, std::move(path), options));
    Status status = db->Initialize();
    if (!status.ok()) return status;
    return db;
  } catch (...) {
    return Status::ResourceExhausted("opening database failed");
  }
}

DB::~DB() { Close(); }

Status DB::Initialize() {
  bool exists = false;
  Status status = env_->FileExists(path_, &exists);
  if (!status.ok()) return status;
  if (!exists && !options_.create_if_missing) {
    return Status::NotFound("database directory does not exist");
  }
  status = env_->CreateDirectories(path_);
  if (!status.ok()) return status;
  status = env_->LockFile(JoinPath(path_, "LOCK"));
  if (!status.ok()) return status;
  locked_ = true;

  auto versions = VersionSet::Open(env_, path_);
  if (!versions.ok()) return versions.status();
  versions_ = std::move(versions).value();
  block_cache_ = std::make_unique<BlockCache>(options_.block_cache_size);
  mutable_memtable_ =
      std::make_shared<MemTable>(options_.arena_block_size);
  if (!mutable_memtable_->status().ok()) return mutable_memtable_->status();

  for (const FileMeta& metadata : versions_->Files()) {
    auto opened = Table::Open(env_, TableFileName(path_, metadata.number),
                              block_cache_.get());
    if (!opened.ok()) return opened.status();
    std::unique_ptr<Table> table = std::move(opened).value();
    if (table->file_size() != metadata.file_size) {
      return Status::Corruption("table size does not match manifest");
    }
    tables_.push_back({metadata, std::shared_ptr<Table>(std::move(table))});
  }

  const std::string log_path = JoinPath(path_, "LOG-000001");
  std::vector<std::string> log_records;
  status = WAL::Recover(env_, log_path, &log_records);
  if (status.code() == StatusCode::kNotFound &&
      versions_->last_sequence() != 0) {
    return Status::Corruption("WAL is missing for a non-empty database");
  }
  if (status.code() != StatusCode::kNotFound && !status.ok()) return status;
  auto log = WAL::Open(env_, log_path, options_.wal_options);
  if (!log.ok()) return log.status();
  wal_ = std::move(log).value();

  std::uint64_t replay_sequence = 0;
  std::vector<LoggedOperation> operations;
  for (const std::string& record : log_records) {
    status = DecodeWriteGroup(record, &operations);
    if (!status.ok()) return status;
    for (const LoggedOperation& operation : operations) {
      if (operation.sequence != replay_sequence + 1) {
        return Status::Corruption("WAL sequence numbers are not contiguous");
      }
      replay_sequence = operation.sequence;
      if (operation.sequence <= versions_->last_sequence()) continue;
      status = mutable_memtable_->Add(operation.sequence, operation.type,
                                      operation.key, operation.value);
      if (!status.ok()) return status;
      if (mutable_memtable_->approximate_size() >= options_.write_buffer_size) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status = RotateMemTableLocked();
        if (!status.ok()) return status;
      }
    }
  }
  if (versions_->last_sequence() > replay_sequence && !log_records.empty()) {
    return Status::Corruption("manifest sequence is newer than the WAL");
  }
  last_sequence_.store(std::max(replay_sequence, versions_->last_sequence()),
                       std::memory_order_release);
  background_thread_ = std::thread(&DB::BackgroundLoop, this);
  return Status::OK();
}

Status DB::Put(std::string_view key, std::string_view value) {
  WriteBatch batch;
  Status status = batch.Put(key, value);
  if (!status.ok()) return status;
  return Write(batch);
}

Status DB::Delete(std::string_view key) {
  WriteBatch batch;
  Status status = batch.Delete(key);
  if (!status.ok()) return status;
  return Write(batch);
}

Status DB::Write(const WriteBatch& batch) {
  if (batch.empty()) return Status::OK();
  WriteRequest request;
  try {
    request.batch = batch;
  } catch (...) {
    return Status::ResourceExhausted("copying write batch failed");
  }
  std::unique_lock<std::mutex> lock(write_mutex_);
  if (closed_.load(std::memory_order_acquire) || closing_) {
    return Status::Busy("database is closed");
  }
  if (!writer_error_.ok()) return writer_error_;
  try {
    write_queue_.push_back(&request);
  } catch (...) {
    return Status::ResourceExhausted("queueing database write failed");
  }
  if (write_leader_) {
    write_condition_.wait(lock, [&request] { return request.done; });
    return request.status;
  }

  write_leader_ = true;
  while (true) {
    std::vector<WriteRequest*> group;
    group.swap(write_queue_);
    Status status = writer_error_;
    if (status.ok()) {
      lock.unlock();
      status = ApplyWriteGroup(group);
      lock.lock();
      if (!status.ok()) writer_error_ = status;
    }
    for (WriteRequest* queued : group) {
      queued->status = writer_error_.ok() ? status : writer_error_;
      queued->done = true;
    }
    write_condition_.notify_all();
    if (write_queue_.empty()) {
      write_leader_ = false;
      write_condition_.notify_all();
      return request.status;
    }
  }
}

Status DB::EncodeWriteGroup(const std::vector<WriteRequest*>& requests,
                            std::uint64_t first_sequence,
                            std::string* encoded,
                            std::uint64_t* last_sequence) {
  if (encoded == nullptr || last_sequence == nullptr) {
    return Status::InvalidArgument("null write group output");
  }
  std::uint64_t count = 0;
  for (const WriteRequest* request : requests) {
    if (request->batch.operations().size() >
        std::numeric_limits<std::uint64_t>::max() - count) {
      return Status::ResourceExhausted("write group has too many operations");
    }
    count += request->batch.operations().size();
  }
  if (count == 0) {
    *last_sequence = first_sequence - 1;
    encoded->clear();
    return Status::OK();
  }
  constexpr std::uint64_t kMaxSequence =
      std::numeric_limits<std::uint64_t>::max() >> 8U;
  if (first_sequence == 0 || first_sequence > kMaxSequence ||
      count - 1 > kMaxSequence - first_sequence) {
    return Status::ResourceExhausted("database sequence number exhausted");
  }
  try {
    std::string result;
    PutFixed32(&result, kWriteBatchFormat);
    PutVarint64(&result, count);
    std::uint64_t sequence = first_sequence;
    for (const WriteRequest* request : requests) {
      for (const WriteBatch::Operation& operation : request->batch.operations()) {
        PutFixed64(&result, sequence++);
        result.push_back(static_cast<char>(operation.type));
        PutLengthPrefixed(&result, operation.key);
        if (operation.type == ValueType::kValue) {
          PutLengthPrefixed(&result, operation.value);
        }
      }
    }
    *encoded = std::move(result);
    *last_sequence = sequence - 1;
  } catch (...) {
    return Status::ResourceExhausted("encoding write group failed");
  }
  return Status::OK();
}

Status DB::WaitForWriteSpace() {
  const auto start = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(state_mutex_);
  bool stalled = false;
  while (true) {
    if (!background_error_.ok()) return background_error_;
    if (background_stopping_) return Status::Busy("database is closing");
    const std::size_t l0_files = static_cast<std::size_t>(std::count_if(
        tables_.begin(), tables_.end(),
        [](const TableState& table) { return table.metadata.level == 0; }));
    const bool immutable_limit =
        immutable_memtables_.size() >= options_.max_immutable_memtables &&
        mutable_memtable_->approximate_size() >= options_.write_buffer_size;
    if (l0_files < options_.l0_stop_writes_trigger && !immutable_limit) {
      if (stalled) {
        write_stall_micros_.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count()),
            std::memory_order_relaxed);
      }
      if (l0_files >= options_.l0_write_slowdown_trigger) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return Status::OK();
    }
    stalled = true;
    background_condition_.wait(lock);
  }
}

Status DB::ApplyWriteGroup(const std::vector<WriteRequest*>& requests) {
  Status status = WaitForWriteSpace();
  if (!status.ok()) return status;
  const std::uint64_t first_sequence =
      last_sequence_.load(std::memory_order_acquire) + 1;
  std::string record;
  std::uint64_t last_sequence = 0;
  status = EncodeWriteGroup(requests, first_sequence, &record, &last_sequence);
  if (!status.ok()) return status;
  if (record.empty()) return Status::OK();
  status = wal_->Append(record);
  if (!status.ok()) return status;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::uint64_t sequence = first_sequence;
    for (const WriteRequest* request : requests) {
      for (const WriteBatch::Operation& operation : request->batch.operations()) {
        status = mutable_memtable_->Add(sequence++, operation.type,
                                        operation.key, operation.value);
        if (!status.ok()) return status;
      }
    }
    last_sequence_.store(last_sequence, std::memory_order_release);
    writes_.fetch_add(sequence - first_sequence, std::memory_order_relaxed);
    if (mutable_memtable_->approximate_size() >= options_.write_buffer_size) {
      status = RotateMemTableLocked();
      if (!status.ok()) return status;
    }
  }
  background_condition_.notify_all();
  return Status::OK();
}

Status DB::RotateMemTableLocked() {
  if (!mutable_memtable_ || mutable_memtable_->last_sequence() == 0) {
    return Status::OK();
  }
  try {
    auto next = std::make_shared<MemTable>(options_.arena_block_size);
    if (!next->status().ok()) return next->status();
    immutable_memtables_.push_back(mutable_memtable_);
    mutable_memtable_ = std::move(next);
  } catch (...) {
    return Status::ResourceExhausted("rotating memtable failed");
  }
  return Status::OK();
}

void DB::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (true) {
    background_condition_.wait(lock, [this] {
      const std::size_t l0_files = static_cast<std::size_t>(std::count_if(
          tables_.begin(), tables_.end(),
          [](const TableState& table) { return table.metadata.level == 0; }));
      return background_stopping_ || !immutable_memtables_.empty() ||
             force_compaction_ ||
             l0_files >= options_.l0_compaction_trigger;
    });
    if (background_stopping_ &&
        (immutable_memtables_.empty() || !background_error_.ok())) {
      break;
    }
    if (!background_error_.ok()) {
      background_condition_.wait(lock,
                                 [this] { return background_stopping_; });
      continue;
    }
    if (!immutable_memtables_.empty()) {
      const auto memtable = immutable_memtables_.front();
      background_busy_ = true;
      lock.unlock();
      Status status = FlushImmutable(memtable);
      lock.lock();
      background_busy_ = false;
      if (!status.ok()) {
        background_error_ = status;
      } else {
        if (immutable_memtables_.empty() ||
            immutable_memtables_.front() != memtable) {
          background_error_ =
              Status::Corruption("immutable memtable queue changed unexpectedly");
        } else {
          immutable_memtables_.erase(immutable_memtables_.begin());
        }
      }
      background_condition_.notify_all();
      continue;
    }
    if (!background_stopping_ &&
        (force_compaction_ ||
         std::count_if(tables_.begin(), tables_.end(),
                       [](const TableState& table) {
                         return table.metadata.level == 0;
                       }) >=
             static_cast<std::ptrdiff_t>(options_.l0_compaction_trigger))) {
      background_busy_ = true;
      force_compaction_ = false;
      lock.unlock();
      Status status = CompactTables();
      lock.lock();
      background_busy_ = false;
      if (!status.ok()) background_error_ = status;
      background_condition_.notify_all();
    }
  }
  background_condition_.notify_all();
}

Status DB::FlushImmutable(const std::shared_ptr<MemTable>& memtable) {
  if (!memtable) return Status::InvalidArgument("null immutable memtable");
  const std::uint64_t number = versions_->NewFileNumber();
  if (number == 0) return Status::ResourceExhausted("table file number exhausted");
  const std::string file_path = TableFileName(path_, number);
  auto builder = TableBuilder::Create(env_, file_path, options_.table_options);
  if (!builder.ok()) return builder.status();
  auto table_builder = std::move(builder).value();
  auto iterator = memtable->NewIterator();
  iterator.SeekToFirst();
  std::string smallest;
  std::string largest;
  while (iterator.Valid()) {
    if (smallest.empty()) smallest.assign(iterator.key());
    largest.assign(iterator.key());
    Status status = table_builder->Add(iterator.key(), iterator.value());
    if (!status.ok()) return status;
    iterator.Next();
  }
  if (!iterator.Valid() && !iterator.key().empty()) {
    return Status::Corruption("immutable memtable iterator failed");
  }
  Status status = table_builder->Finish();
  if (!status.ok()) return status;
  auto opened = Table::Open(env_, file_path, block_cache_.get());
  if (!opened.ok()) return opened.status();
  std::shared_ptr<Table> table(
      std::move(opened).value());
  FileMeta metadata{number, 0, table_builder->file_size(), smallest, largest};
  VersionEdit edit;
  edit.last_sequence = memtable->last_sequence();
  edit.next_file_number = versions_->next_file_number();
  edit.added_files.push_back(metadata);
  status = versions_->LogAndApply(edit);
  if (!status.ok()) return status;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    tables_.push_back({metadata, std::move(table)});
  }
  background_condition_.notify_all();
  return Status::OK();
}

Status DB::CompactTables() {
  std::vector<TableState> input_tables;
  std::uint64_t oldest_snapshot = 0;
  std::uint64_t latest_sequence = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    input_tables = tables_;
    latest_sequence = versions_->last_sequence();
    oldest_snapshot = latest_sequence;
    for (const auto& [id, sequence] : snapshots_) {
      static_cast<void>(id);
      oldest_snapshot = std::min(oldest_snapshot, sequence);
    }
  }
  if (input_tables.empty()) return Status::OK();

  std::vector<std::pair<std::string, std::string>> entries;
  try {
    for (const TableState& table : input_tables) {
      Status status = table.table->ReadEntries(&entries);
      if (!status.ok()) return status;
      compaction_bytes_read_.fetch_add(table.metadata.file_size,
                                       std::memory_order_relaxed);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) {
                return LexicographicComparator()(lhs.first, rhs.first) < 0;
              });
  } catch (...) {
    return Status::ResourceExhausted("merging table entries failed");
  }

  std::vector<std::pair<std::string, std::string>> retained;
  std::size_t cursor = 0;
  while (cursor < entries.size()) {
    std::string user_key;
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kDeletion;
    Status status = DecodeInternalKey(entries[cursor].first, &user_key,
                                      &sequence, &type);
    if (!status.ok()) return status;
    const std::string group_key = user_key;
    bool base_version_selected = false;
    std::string previous_internal_key;
    while (cursor < entries.size()) {
      std::string current_user_key;
      std::uint64_t current_sequence = 0;
      ValueType current_type = ValueType::kDeletion;
      status = DecodeInternalKey(entries[cursor].first, &current_user_key,
                                 &current_sequence, &current_type);
      if (!status.ok()) return status;
      if (current_user_key != group_key) break;
      if (entries[cursor].first != previous_internal_key) {
        if (current_sequence > oldest_snapshot) {
          retained.push_back(entries[cursor]);
        } else if (!base_version_selected) {
          base_version_selected = true;
          if (current_type == ValueType::kValue) {
            retained.push_back(entries[cursor]);
          }
        }
        previous_internal_key = entries[cursor].first;
      }
      ++cursor;
    }
  }

  std::uint64_t output_number = 0;
  std::uint64_t output_size = 0;
  std::string output_smallest;
  std::string output_largest;
  std::shared_ptr<Table> output_table;
  if (!retained.empty()) {
    output_number = versions_->NewFileNumber();
    if (output_number == 0) {
      return Status::ResourceExhausted("table file number exhausted");
    }
    const std::string output_path = TableFileName(path_, output_number);
    auto builder =
        TableBuilder::Create(env_, output_path, options_.table_options);
    if (!builder.ok()) return builder.status();
    auto table_builder = std::move(builder).value();
    for (std::size_t i = 0; i < retained.size(); ++i) {
      if (i == 0) output_smallest = retained[i].first;
      output_largest = retained[i].first;
      Status status = table_builder->Add(retained[i].first, retained[i].second);
      if (!status.ok()) return status;
    }
    Status status = table_builder->Finish();
    if (!status.ok()) return status;
    output_size = table_builder->file_size();
    auto opened = Table::Open(env_, output_path, block_cache_.get());
    if (!opened.ok()) return opened.status();
    output_table = std::shared_ptr<Table>(std::move(opened).value());
    compaction_bytes_written_.fetch_add(output_size,
                                        std::memory_order_relaxed);
  }

  VersionEdit edit;
  edit.last_sequence = latest_sequence;
  if (output_number != 0) {
    edit.added_files.push_back(
        {output_number, 1, output_size, output_smallest, output_largest});
  }
  for (const TableState& table : input_tables) {
    edit.deleted_files.emplace_back(table.metadata.level,
                                    table.metadata.number);
  }
  edit.next_file_number = versions_->next_file_number();
  Status status = versions_->LogAndApply(edit);
  if (!status.ok()) return status;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    tables_.clear();
    if (output_table) {
      tables_.push_back(
          {{output_number, 1, output_size, output_smallest, output_largest},
           output_table});
    }
  }
  background_condition_.notify_all();
  for (const TableState& table : input_tables) {
    status = env_->RemoveFile(TableFileName(path_, table.metadata.number));
    if (!status.ok() && status.code() != StatusCode::kNotFound) return status;
  }
  return Status::OK();
}

Status DB::ValidateSnapshotLocked(const Snapshot* snapshot,
                                  std::uint64_t* sequence) const {
  if (sequence == nullptr) {
    return Status::InvalidArgument("null snapshot sequence output");
  }
  if (snapshot == nullptr) {
    *sequence = last_sequence_.load(std::memory_order_acquire);
    return Status::OK();
  }
  if (snapshot->database_id != database_id_) {
    return Status::InvalidArgument("snapshot belongs to another database");
  }
  const auto found = snapshots_.find(snapshot->id);
  if (found == snapshots_.end() || found->second != snapshot->sequence) {
    return Status::InvalidArgument("snapshot is not active");
  }
  *sequence = snapshot->sequence;
  return Status::OK();
}

Status DB::Get(std::string_view key, std::string* value,
               const Snapshot* snapshot) const {
  if (value == nullptr) return Status::InvalidArgument("null database value output");
  if (closed_.load(std::memory_order_acquire)) {
    return Status::Busy("database is closed");
  }
  std::uint64_t sequence = 0;
  std::shared_ptr<MemTable> mutable_memtable;
  std::vector<std::shared_ptr<MemTable>> immutable_memtables;
  std::vector<TableState> tables;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    Status status = ValidateSnapshotLocked(snapshot, &sequence);
    if (!status.ok()) return status;
    mutable_memtable = mutable_memtable_;
    immutable_memtables = immutable_memtables_;
    tables = tables_;
  }
  reads_.fetch_add(1, std::memory_order_relaxed);
  if (sequence == 0) return Status::NotFound("key not found");
  std::string best_value;
  std::uint64_t best_sequence = 0;
  ValueType best_type = ValueType::kDeletion;
  const auto consider_memtable =
      [&](const std::shared_ptr<MemTable>& table) -> Status {
    if (!table) return Status::OK();
    std::string candidate_value;
    std::uint64_t candidate_sequence = 0;
    ValueType candidate_type = ValueType::kDeletion;
    Status status = table->GetEntry(key, sequence, &candidate_value,
                                    &candidate_sequence, &candidate_type);
    if (status.code() == StatusCode::kNotFound) return Status::OK();
    if (!status.ok()) return status;
    if (candidate_sequence > best_sequence) {
      best_sequence = candidate_sequence;
      best_type = candidate_type;
      best_value = std::move(candidate_value);
    }
    return Status::OK();
  };
  Status status = consider_memtable(mutable_memtable);
  if (!status.ok()) return status;
  for (const auto& memtable : immutable_memtables) {
    status = consider_memtable(memtable);
    if (!status.ok()) return status;
  }

  std::string search_key;
  status = EncodeInternalKey(key, sequence, ValueType::kDeletion, &search_key);
  if (!status.ok()) return status;
  for (const TableState& table : tables) {
    std::string candidate_key;
    std::string candidate_value;
    status = table.table->GetAtOrAfter(search_key, &candidate_key,
                                       &candidate_value);
    if (status.code() == StatusCode::kNotFound) continue;
    if (!status.ok()) return status;
    std::string candidate_user_key;
    std::uint64_t candidate_sequence = 0;
    ValueType candidate_type = ValueType::kDeletion;
    status = DecodeInternalKey(candidate_key, &candidate_user_key,
                               &candidate_sequence, &candidate_type);
    if (!status.ok()) return status;
    if (candidate_user_key != key || candidate_sequence > sequence) continue;
    if (candidate_sequence > best_sequence) {
      best_sequence = candidate_sequence;
      best_type = candidate_type;
      best_value = std::move(candidate_value);
    }
  }
  if (best_sequence == 0 || best_type == ValueType::kDeletion) {
    return Status::NotFound("key not found");
  }
  try {
    *value = std::move(best_value);
  } catch (...) {
    return Status::ResourceExhausted("returning database value failed");
  }
  return Status::OK();
}

StatusOr<Snapshot> DB::GetSnapshot() {
  if (closed_.load(std::memory_order_acquire)) {
    return Status::Busy("database is closed");
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (background_stopping_) return Status::Busy("database is closing");
  if (next_snapshot_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return Status::ResourceExhausted("snapshot id space exhausted");
  }
  const std::uint64_t id = next_snapshot_id_++;
  const std::uint64_t sequence = last_sequence_.load(std::memory_order_acquire);
  try {
    snapshots_.emplace(id, sequence);
  } catch (...) {
    return Status::ResourceExhausted("registering snapshot failed");
  }
  return Snapshot{database_id_, id, sequence};
}

Status DB::ReleaseSnapshot(const Snapshot& snapshot) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (snapshot.database_id != database_id_) {
    return Status::InvalidArgument("snapshot belongs to another database");
  }
  const auto found = snapshots_.find(snapshot.id);
  if (found == snapshots_.end() || found->second != snapshot.sequence) {
    return Status::InvalidArgument("snapshot is not active");
  }
  snapshots_.erase(found);
  background_condition_.notify_all();
  return Status::OK();
}

Status DB::CollectMemTableEntries(
    const std::shared_ptr<MemTable>& memtable, std::uint64_t snapshot,
    std::map<std::string, LatestValue, UserKeyLess>* latest) const {
  if (memtable == nullptr || latest == nullptr) {
    return Status::InvalidArgument("invalid memtable iterator arguments");
  }
  auto iterator = memtable->NewIterator();
  iterator.SeekToFirst();
  while (iterator.Valid()) {
    std::string user_key;
    std::uint64_t sequence = 0;
    ValueType type = ValueType::kDeletion;
    Status status =
        DecodeInternalKey(iterator.key(), &user_key, &sequence, &type);
    if (!status.ok()) return status;
    if (sequence <= snapshot) {
      auto found = latest->find(user_key);
      if (found == latest->end() || sequence > found->second.sequence) {
        try {
          latest->insert_or_assign(
              std::move(user_key),
              LatestValue{sequence, type, std::string(iterator.value())});
        } catch (...) {
          return Status::ResourceExhausted("building database iterator failed");
        }
      }
    }
    iterator.Next();
  }
  return Status::OK();
}

StatusOr<std::unique_ptr<DBIterator>> DB::NewIterator(
    const Snapshot* snapshot) const {
  if (closed_.load(std::memory_order_acquire)) {
    return Status::Busy("database is closed");
  }
  std::uint64_t sequence = 0;
  std::shared_ptr<MemTable> mutable_memtable;
  std::vector<std::shared_ptr<MemTable>> immutable_memtables;
  std::vector<TableState> tables;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    Status status = ValidateSnapshotLocked(snapshot, &sequence);
    if (!status.ok()) return status;
    mutable_memtable = mutable_memtable_;
    immutable_memtables = immutable_memtables_;
    tables = tables_;
  }
  std::map<std::string, LatestValue, UserKeyLess> latest;
  Status status = CollectMemTableEntries(mutable_memtable, sequence, &latest);
  if (!status.ok()) return status;
  for (const auto& memtable : immutable_memtables) {
    status = CollectMemTableEntries(memtable, sequence, &latest);
    if (!status.ok()) return status;
  }
  try {
    for (const TableState& table : tables) {
      std::vector<std::pair<std::string, std::string>> entries;
      status = table.table->ReadEntries(&entries);
      if (!status.ok()) return status;
      for (auto& [internal_key, value] : entries) {
        std::string user_key;
        std::uint64_t entry_sequence = 0;
        ValueType type = ValueType::kDeletion;
        status = DecodeInternalKey(internal_key, &user_key, &entry_sequence,
                                   &type);
        if (!status.ok()) return status;
        if (entry_sequence <= sequence) {
          auto found = latest.find(user_key);
          if (found == latest.end() ||
              entry_sequence > found->second.sequence) {
            latest.insert_or_assign(
                std::move(user_key),
                LatestValue{entry_sequence, type, std::move(value)});
          }
        }
      }
    }
    std::vector<std::pair<std::string, std::string>> visible;
    visible.reserve(latest.size());
    for (auto& [key, value] : latest) {
      if (value.type == ValueType::kValue) {
        visible.emplace_back(key, std::move(value.value));
      }
    }
    return std::unique_ptr<DBIterator>(new DBIterator(std::move(visible)));
  } catch (...) {
    return Status::ResourceExhausted("building database iterator failed");
  }
}

Status DB::Compact() {
  if (closed_.load(std::memory_order_acquire)) {
    return Status::Busy("database is closed");
  }
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (!background_error_.ok()) return background_error_;
  if (background_stopping_) return Status::Busy("database is closing");
  if (mutable_memtable_ && mutable_memtable_->last_sequence() != 0) {
    Status status = RotateMemTableLocked();
    if (!status.ok()) return status;
  }
  force_compaction_ = true;
  background_condition_.notify_all();
  background_condition_.wait(lock, [this] {
    return !background_error_.ok() ||
           (immutable_memtables_.empty() && !background_busy_ &&
            !force_compaction_);
  });
  return background_error_;
}

Status DB::Close() {
  std::lock_guard<std::mutex> close_lock(close_mutex_);
  if (closed_.load(std::memory_order_acquire)) return close_status_;
  {
    std::unique_lock<std::mutex> lock(write_mutex_);
    closing_ = true;
    write_condition_.notify_all();
    write_condition_.wait(lock, [this] { return !write_leader_; });
  }
  {
    std::unique_lock<std::mutex> lock(state_mutex_);
    const bool worker_started = background_thread_.joinable();
    if (worker_started && mutable_memtable_ &&
        mutable_memtable_->last_sequence() != 0 &&
        background_error_.ok()) {
      Status status = RotateMemTableLocked();
      if (!status.ok()) close_status_ = status;
    }
    background_condition_.notify_all();
    if (worker_started) {
      background_condition_.wait(lock, [this] {
        return immutable_memtables_.empty() || !background_error_.ok();
      });
    }
    if (close_status_.ok() && !background_error_.ok()) {
      close_status_ = background_error_;
    }
    background_stopping_ = true;
    background_condition_.notify_all();
  }
  if (background_thread_.joinable()) background_thread_.join();
  if (locked_) {
    Status status = env_->UnlockFile(JoinPath(path_, "LOCK"));
    if (close_status_.ok() && !status.ok()) close_status_ = status;
    locked_ = false;
  }
  closed_.store(true, std::memory_order_release);
  return close_status_;
}

StatusOr<std::string> DB::GetProperty(std::string_view property) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (property == "microfs.num-files-at-level0" ||
      property == "microfs.num-files-at-level1") {
    const std::uint32_t level =
        property == "microfs.num-files-at-level0" ? 0 : 1;
    const auto count = std::count_if(
        tables_.begin(), tables_.end(), [level](const TableState& table) {
          return table.metadata.level == level;
        });
    return std::to_string(count);
  }
  if (property == "microfs.wal-fsyncs") {
    return std::to_string(wal_ == nullptr ? 0 : wal_->fsync_count());
  }
  if (property == "microfs.stats") {
    return "reads=" + std::to_string(reads_.load(std::memory_order_relaxed)) +
           " writes=" +
           std::to_string(writes_.load(std::memory_order_relaxed)) +
           " tables=" + std::to_string(tables_.size()) +
           " wal_fsyncs=" +
           std::to_string(wal_ == nullptr ? 0 : wal_->fsync_count()) +
           " cache_hits=" +
           std::to_string(block_cache_ == nullptr ? 0 : block_cache_->hits()) +
           " cache_misses=" +
           std::to_string(block_cache_ == nullptr ? 0
                                                  : block_cache_->misses()) +
           " compaction_bytes_read=" +
           std::to_string(
               compaction_bytes_read_.load(std::memory_order_relaxed)) +
           " compaction_bytes_written=" +
           std::to_string(
               compaction_bytes_written_.load(std::memory_order_relaxed)) +
           " write_stall_us=" +
           std::to_string(write_stall_micros_.load(std::memory_order_relaxed));
  }
  return Status::NotFound("unknown database property");
}

}  // namespace microfs
