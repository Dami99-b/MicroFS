#include "microfs/arena.h"
#include "microfs/block.h"
#include "microfs/block_cache.h"
#include "microfs/bloom_filter.h"
#include "microfs/coding.h"
#include "microfs/crc32c.h"
#include "microfs/db.h"
#include "microfs/env.h"
#include "microfs/hash.h"
#include "microfs/histogram.h"
#include "microfs/internal_key.h"
#include "microfs/memtable.h"
#include "microfs/skiplist.h"
#include "microfs/status.h"
#include "microfs/table.h"
#include "microfs/tick_store.h"
#include "microfs/version_set.h"
#include "test_harness.h"
#include "microfs/wal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::string TempPath(std::string_view name) {
  return (std::filesystem::temp_directory_path() /
          ("microfs-phase1-" + std::string(name)))
      .string();
}

MICROFS_TEST(StatusAndStatusOr) {
  const microfs::Status ok;
  MICROFS_CHECK(ok.ok());
  MICROFS_CHECK(microfs::Status::NotFound("missing").code() ==
                microfs::StatusCode::kNotFound);
  microfs::StatusOr<int> value(42);
  MICROFS_CHECK(value.ok());
  MICROFS_CHECK(value.value() == 42);
}

MICROFS_TEST(FixedAndVariableCoding) {
  std::string encoded;
  microfs::PutFixed32(&encoded, 0x78563412U);
  microfs::PutFixed64(&encoded, 0xfedcba9876543210ULL);
  MICROFS_CHECK(microfs::DecodeFixed32(encoded.data()) == 0x78563412U);
  MICROFS_CHECK(microfs::DecodeFixed64(encoded.data() + 4) ==
                0xfedcba9876543210ULL);

  for (const std::uint64_t input :
       {0ULL, 1ULL, 127ULL, 128ULL, 16384ULL, UINT64_MAX}) {
    encoded.clear();
    microfs::PutVarint64(&encoded, input);
    std::string_view view(encoded);
    std::uint64_t output = 0;
    MICROFS_CHECK(microfs::GetVarint64(&view, &output));
    MICROFS_CHECK(output == input);
    MICROFS_CHECK(view.empty());
  }

  encoded.clear();
  microfs::PutLengthPrefixed(&encoded, "microfs");
  std::string_view view(encoded);
  std::string_view result;
  MICROFS_CHECK(microfs::GetLengthPrefixed(&view, &result));
  MICROFS_CHECK(result == "microfs");
  MICROFS_CHECK(!microfs::GetLengthPrefixed(&view, &result));
}

MICROFS_TEST(Crc32cKnownVectors) {
  MICROFS_CHECK(microfs::Crc32c("123456789", 9) == 0xe3069283U);
  MICROFS_CHECK(microfs::Crc32c("", 0) == 0U);
}

MICROFS_TEST(MurmurHashKnownVector) {
  MICROFS_CHECK(microfs::MurmurHash3("", 0) == 0U);
  MICROFS_CHECK(microfs::MurmurHash3("foo", 0) == 0xf6a5c420U);
}

MICROFS_TEST(ArenaAlignmentAndBlocks) {
  microfs::Arena arena(128);
  auto* first = static_cast<std::byte*>(arena.Allocate(17, 16));
  auto* second = static_cast<std::byte*>(arena.Allocate(32, 32));
  MICROFS_CHECK(first != nullptr);
  MICROFS_CHECK(second != nullptr);
  MICROFS_CHECK(reinterpret_cast<std::uintptr_t>(first) % 16 == 0);
  MICROFS_CHECK(reinterpret_cast<std::uintptr_t>(second) % 32 == 0);
  MICROFS_CHECK(arena.allocated_bytes() >= 128);
  MICROFS_CHECK(arena.Allocate(1, 3) == nullptr);
}

MICROFS_TEST(HistogramPercentiles) {
  microfs::Histogram histogram;
  for (std::uint64_t value = 1; value <= 1000; ++value) {
    histogram.Record(value);
  }
  MICROFS_CHECK(histogram.count() == 1000);
  MICROFS_CHECK(histogram.max() == 1000);
  MICROFS_CHECK(histogram.Percentile(50) >= 500);
  MICROFS_CHECK(histogram.Percentile(99) >= 990);
  MICROFS_CHECK(histogram.Percentile(100) == 1000);
}

MICROFS_TEST(StandardEnvironmentRoundTrip) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("standard");
  const std::array<std::byte, 3> input{std::byte{1}, std::byte{2},
                                       std::byte{3}};
  MICROFS_CHECK(env->WriteFile(path, input).ok());
  MICROFS_CHECK(env->SyncFile(path).ok());
  std::vector<std::byte> output;
  MICROFS_CHECK(env->ReadFile(path, &output).ok());
  MICROFS_CHECK(output == std::vector<std::byte>(input.begin(), input.end()));
  MICROFS_CHECK(env->RemoveFile(path).ok());
}

MICROFS_TEST(FaultEnvironmentCrashAndTear) {
  auto base = microfs::NewDefaultEnv();
  const std::string path = TempPath("fault");
  const std::array<std::byte, 3> original{std::byte{1}, std::byte{2},
                                          std::byte{3}};
  MICROFS_CHECK(base->WriteFile(path, original).ok());
  MICROFS_CHECK(base->SyncFile(path).ok());

  microfs::FaultInjectingEnv env(std::move(base));
  const std::array<std::byte, 2> replacement{std::byte{4}, std::byte{5}};
  MICROFS_CHECK(env.WriteFile(path, replacement).ok());
  env.Crash();
  std::vector<std::byte> output;
  MICROFS_CHECK(env.ReadFile(path, &output).ok());
  MICROFS_CHECK(output == std::vector<std::byte>(original.begin(), original.end()));

  env.TearNextWrite(1);
  MICROFS_CHECK(env.WriteFile(path, replacement).ok());
  MICROFS_CHECK(env.SyncFile(path).ok());
  MICROFS_CHECK(env.ReadFile(path, &output).ok());
  MICROFS_CHECK(output.size() == 1);
  MICROFS_CHECK(output[0] == std::byte{4});
  MICROFS_CHECK(env.RemoveFile(path).ok());
}

MICROFS_TEST(SkipListOrderingAndIterator) {
  microfs::SkipList<> list;
  MICROFS_CHECK(list.status().ok());
  MICROFS_CHECK(list.Insert("key-2", "two").ok());
  MICROFS_CHECK(list.Insert("key-1", "one").ok());
  MICROFS_CHECK(list.Insert("key-3", "three").ok());
  MICROFS_CHECK(!list.Insert("key-2", "duplicate").ok());

  auto iterator = list.NewIterator();
  iterator.Seek("key-2");
  MICROFS_CHECK(iterator.Valid());
  MICROFS_CHECK(iterator.key() == "key-2");
  MICROFS_CHECK(iterator.value() == "two");
  iterator.Prev();
  MICROFS_CHECK(iterator.key() == "key-1");
  iterator.SeekToLast();
  MICROFS_CHECK(iterator.key() == "key-3");
  iterator.Next();
  MICROFS_CHECK(!iterator.Valid());
  iterator.SeekToFirst();
  MICROFS_CHECK(iterator.key() == "key-1");
}

MICROFS_TEST(SkipListConcurrentReaders) {
  microfs::SkipList<> list;
  std::atomic<bool> done{false};
  std::atomic<std::size_t> failures{0};
  std::mutex failure_mutex;
  std::string first_target;
  std::string first_found;
  std::vector<std::thread> readers;
  for (int reader = 0; reader < 3; ++reader) {
    readers.emplace_back([&] {
      std::size_t probe = 0;
      while (!done.load(std::memory_order_acquire) || probe < 5000) {
        const std::string target = "key-" + std::to_string(probe % 2500);
        const auto* node = list.FindGreaterOrEqual(target);
        if (node != nullptr &&
            microfs::LexicographicComparator()(node->key, target) < 0) {
          if (failures.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> lock(failure_mutex);
            first_target = target;
            first_found = std::string(node->key);
          }
        }
        ++probe;
      }
    });
  }
  for (std::size_t i = 0; i < 2500; ++i) {
    const std::string key = "key-" + std::to_string(i);
    MICROFS_CHECK(list.Insert(key, "value").ok());
  }
  done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) reader.join();
  if (failures.load(std::memory_order_relaxed) != 0) {
    std::cerr << "lookup for '" << first_target << "' returned '"
              << first_found << "'\n";
  }
  MICROFS_CHECK(failures.load(std::memory_order_relaxed) == 0);
  auto iterator = list.NewIterator();
  iterator.SeekToFirst();
  std::size_t visited = 0;
  std::string previous;
  while (iterator.Valid()) {
    if (visited != 0) {
      MICROFS_CHECK(
          microfs::LexicographicComparator()(previous, iterator.key()) < 0);
    }
    previous.assign(iterator.key());
    ++visited;
    iterator.Next();
  }
  MICROFS_CHECK(visited == 2500);
}

MICROFS_TEST(MemTableSnapshotAndDeletion) {
  microfs::MemTable table;
  const std::string key("a\0b", 3);
  MICROFS_CHECK(table.status().ok());
  MICROFS_CHECK(table.Add(1, microfs::ValueType::kValue, key, "old").ok());
  MICROFS_CHECK(table.Add(2, microfs::ValueType::kValue, key, "new").ok());
  auto old_value = table.Get(key, 1);
  auto new_value = table.Get(key, 2);
  MICROFS_CHECK(old_value.ok() && old_value.value() == "old");
  MICROFS_CHECK(new_value.ok() && new_value.value() == "new");
  MICROFS_CHECK(table.Add(3, microfs::ValueType::kDeletion, key).ok());
  MICROFS_CHECK(table.Get(key, 2).ok());
  MICROFS_CHECK(table.Get(key, 3).status().code() ==
                microfs::StatusCode::kNotFound);
  MICROFS_CHECK(table.Get("missing", 3).status().code() ==
                microfs::StatusCode::kNotFound);
}

MICROFS_TEST(InternalKeyByteOrderAndBinaryRoundTrip) {
  std::string newest;
  std::string older;
  std::string longer_user_key;
  MICROFS_CHECK(microfs::EncodeInternalKey("a", 9, microfs::ValueType::kValue,
                                           &newest)
                    .ok());
  MICROFS_CHECK(microfs::EncodeInternalKey("a", 3, microfs::ValueType::kValue,
                                           &older)
                    .ok());
  const std::string binary_key("a\0", 2);
  MICROFS_CHECK(microfs::EncodeInternalKey(
                    binary_key, 1, microfs::ValueType::kValue,
                    &longer_user_key)
                    .ok());
  MICROFS_CHECK(microfs::LexicographicComparator()(newest, older) < 0);
  MICROFS_CHECK(microfs::LexicographicComparator()(older, longer_user_key) < 0);
  std::string decoded_key;
  std::uint64_t sequence = 0;
  microfs::ValueType type = microfs::ValueType::kDeletion;
  MICROFS_CHECK(
      microfs::DecodeInternalKey(longer_user_key, &decoded_key, &sequence, &type)
          .ok());
  MICROFS_CHECK(decoded_key == binary_key);
  MICROFS_CHECK(sequence == 1);
  MICROFS_CHECK(type == microfs::ValueType::kValue);
}

MICROFS_TEST(WalFragmentationAndTailRecovery) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("wal-recovery");
  std::error_code error;
  std::filesystem::remove(path, error);
  std::string large(100000, 'x');
  {
    auto opened = microfs::WAL::Open(env.get(), path);
    MICROFS_CHECK(opened.ok());
    auto wal = std::move(opened).value();
    MICROFS_CHECK(wal->Append("").ok());
    MICROFS_CHECK(wal->Append("small record").ok());
    MICROFS_CHECK(wal->Append(large).ok());
    MICROFS_CHECK(wal->fsync_count() == 3);
  }

  std::vector<std::string> records;
  std::uint64_t truncated = 99;
  MICROFS_CHECK(microfs::WAL::Recover(env.get(), path, &records, &truncated).ok());
  MICROFS_CHECK(truncated == 0);
  MICROFS_CHECK(records.size() == 3);
  MICROFS_CHECK(records[0].empty());
  MICROFS_CHECK(records[1] == "small record");
  MICROFS_CHECK(records[2] == large);

  const std::array<std::byte, 3> torn{std::byte{0xff}, std::byte{0xff},
                                      std::byte{0xff}};
  MICROFS_CHECK(env->AppendFile(path, torn).ok());
  MICROFS_CHECK(microfs::WAL::Recover(env.get(), path, &records, &truncated).ok());
  MICROFS_CHECK(truncated == torn.size());
  MICROFS_CHECK(records.size() == 3);
  MICROFS_CHECK(records[2] == large);

  std::vector<std::byte> corrupted_file;
  MICROFS_CHECK(env->ReadFile(path, &corrupted_file).ok());
  corrupted_file[0] ^= std::byte{1};
  MICROFS_CHECK(env->WriteFile(path, corrupted_file).ok());
  MICROFS_CHECK(env->SyncFile(path).ok());
  MICROFS_CHECK(microfs::WAL::Recover(env.get(), path, &records, &truncated).ok());
  MICROFS_CHECK(truncated == corrupted_file.size());
  MICROFS_CHECK(records.empty());
  MICROFS_CHECK(env->RemoveFile(path).ok());
}

MICROFS_TEST(WalSyncPoliciesAndUnsyncedCrash) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("wal-sync");
  std::error_code error;
  std::filesystem::remove(path, error);
  microfs::WalOptions options;
  options.sync_policy = microfs::SyncPolicy::kEveryNBytes;
  options.sync_bytes = 32;
  auto opened = microfs::WAL::Open(env.get(), path, options);
  MICROFS_CHECK(opened.ok());
  auto wal = std::move(opened).value();
  MICROFS_CHECK(wal->Append("small").ok());
  MICROFS_CHECK(wal->fsync_count() == 0);
  MICROFS_CHECK(wal->Append(std::string(100, 'x')).ok());
  MICROFS_CHECK(wal->fsync_count() == 1);
  wal.reset();
  MICROFS_CHECK(env->RemoveFile(path).ok());

  microfs::FaultInjectingEnv fault_env(microfs::NewDefaultEnv());
  options.sync_policy = microfs::SyncPolicy::kNever;
  opened = microfs::WAL::Open(&fault_env, path, options);
  MICROFS_CHECK(opened.ok());
  wal = std::move(opened).value();
  MICROFS_CHECK(wal->Append("unsynced").ok());
  fault_env.Crash();
  wal.reset();
  std::vector<std::string> records;
  MICROFS_CHECK(microfs::WAL::Recover(&fault_env, path, &records).code() ==
                microfs::StatusCode::kNotFound);
}

MICROFS_TEST(WalConcurrentWriters) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("wal-concurrent");
  std::error_code error;
  std::filesystem::remove(path, error);
  auto opened = microfs::WAL::Open(env.get(), path);
  MICROFS_CHECK(opened.ok());
  auto wal = std::move(opened).value();
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kWrites = 40;
  std::atomic<std::size_t> failures{0};
  std::vector<std::thread> writers;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    writers.emplace_back([&, thread] {
      for (std::size_t write = 0; write < kWrites; ++write) {
        const std::string record =
            std::to_string(thread) + ":" + std::to_string(write);
        if (!wal->Append(record).ok()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& writer : writers) writer.join();
  MICROFS_CHECK(failures.load(std::memory_order_relaxed) == 0);
  MICROFS_CHECK(wal->fsync_count() > 0);
  std::vector<std::string> records;
  MICROFS_CHECK(microfs::WAL::Recover(env.get(), path, &records).ok());
  MICROFS_CHECK(records.size() == kThreads * kWrites);
  MICROFS_CHECK(env->RemoveFile(path).ok());
}

MICROFS_TEST(WalPropagatesAndRetainsWriteFailures) {
  const std::string path = TempPath("wal-failure");
  microfs::FaultInjectingEnv env(microfs::NewDefaultEnv());
  auto opened = microfs::WAL::Open(&env, path);
  MICROFS_CHECK(opened.ok());
  auto wal = std::move(opened).value();
  env.FailAfter(0);
  MICROFS_CHECK(wal->Append("failed append").code() ==
                microfs::StatusCode::kIoError);
  env.DisableFailures();
  MICROFS_CHECK(wal->Append("must not append after partial I/O").code() ==
                microfs::StatusCode::kIoError);
  env.Crash();
  wal.reset();
  std::vector<std::string> records;
  MICROFS_CHECK(microfs::WAL::Recover(&env, path, &records).code() ==
                microfs::StatusCode::kNotFound);
}

MICROFS_TEST(BlockPrefixCompressionAndIteration) {
  microfs::BlockBuilder builder(2);
  MICROFS_CHECK(builder.Add("", "empty").ok());
  MICROFS_CHECK(builder.Add("a", "one").ok());
  MICROFS_CHECK(builder.Add("aa", "two").ok());
  MICROFS_CHECK(builder.Add("ab", "three").ok());
  MICROFS_CHECK(builder.Add("b", "four").ok());
  std::string encoded;
  MICROFS_CHECK(builder.Finish(&encoded).ok());
  MICROFS_CHECK(encoded.size() < 5 * (3 + 1 + 8));

  microfs::BlockReader reader;
  MICROFS_CHECK(reader.Open(encoded).ok());
  reader.SeekToFirst();
  MICROFS_CHECK(reader.Valid() && reader.key().empty());
  MICROFS_CHECK(reader.value() == "empty");
  reader.Next();
  MICROFS_CHECK(reader.key() == "a");
  reader.Seek("az");
  MICROFS_CHECK(reader.Valid() && reader.key() == "b");
  reader.Prev();
  MICROFS_CHECK(reader.key() == "ab");
  reader.SeekToLast();
  MICROFS_CHECK(reader.key() == "b");
  reader.Next();
  MICROFS_CHECK(!reader.Valid());

  std::string corrupt = encoded;
  corrupt[corrupt.size() - 1] = 0;
  corrupt[corrupt.size() - 2] = 0;
  corrupt[corrupt.size() - 3] = 0;
  corrupt[corrupt.size() - 4] = 0;
  MICROFS_CHECK(!reader.Open(corrupt).ok());
  MICROFS_CHECK(!builder.Add("aa", "duplicate").ok());

  microfs::BlockBuilder empty_builder;
  MICROFS_CHECK(empty_builder.Finish(&encoded).ok());
  MICROFS_CHECK(reader.Open(encoded).ok());
  reader.SeekToFirst();
  MICROFS_CHECK(!reader.Valid());
}

MICROFS_TEST(BloomFilterFalsePositiveAndRoundTrip) {
  constexpr std::size_t kEntries = 10000;
  auto created = microfs::BloomFilter::Create(kEntries, 10);
  MICROFS_CHECK(created.ok());
  microfs::BloomFilter filter = std::move(created).value();
  for (std::size_t i = 0; i < kEntries; ++i) {
    MICROFS_CHECK(filter.Add("present-" + std::to_string(i)).ok());
  }
  for (std::size_t i = 0; i < kEntries; ++i) {
    MICROFS_CHECK(filter.MayContain("present-" + std::to_string(i)));
  }
  std::size_t false_positives = 0;
  for (std::size_t i = 0; i < kEntries; ++i) {
    if (filter.MayContain("absent-" + std::to_string(i))) {
      ++false_positives;
    }
  }
  MICROFS_CHECK(false_positives * 100 < kEntries * 2);

  std::string encoded;
  MICROFS_CHECK(filter.Encode(&encoded).ok());
  auto decoded = microfs::BloomFilter::Decode(encoded);
  MICROFS_CHECK(decoded.ok());
  for (std::size_t i = 0; i < kEntries; ++i) {
    MICROFS_CHECK(decoded.value().MayContain("present-" + std::to_string(i)));
  }
  encoded.pop_back();
  MICROFS_CHECK(!microfs::BloomFilter::Decode(encoded).ok());
}

MICROFS_TEST(BlockCacheLruAndCounters) {
  microfs::BlockCache cache(1024, 4);
  auto first = std::make_shared<const std::string>("first");
  MICROFS_CHECK(cache.Insert(1, 10, first).ok());
  MICROFS_CHECK(!cache.Get(1, 11));
  MICROFS_CHECK(cache.Get(1, 10) == first);
  MICROFS_CHECK(cache.hits() == 1);
  MICROFS_CHECK(cache.misses() == 1);
  MICROFS_CHECK(cache.hit_rate() == 0.5);
  MICROFS_CHECK(!cache.Insert(1, 12, nullptr).ok());
}

MICROFS_TEST(TableBuildReadCacheAndCorruption) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("table");
  std::error_code error;
  std::filesystem::remove(path, error);
  microfs::TableOptions options;
  options.data_block_size = 100;
  options.restart_interval = 3;
  options.prefix_length = 1;
  auto created = microfs::TableBuilder::Create(env.get(), path, options);
  MICROFS_CHECK(created.ok());
  auto builder = std::move(created).value();
  for (std::size_t i = 0; i < 80; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "S%04zu", i);
    MICROFS_CHECK(builder->Add(key, "value-" + std::to_string(i)).ok());
  }
  for (std::size_t i = 0; i < 20; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "T%04zu", i);
    MICROFS_CHECK(builder->Add(key, "other-" + std::to_string(i)).ok());
  }
  MICROFS_CHECK(builder->Finish().ok());

  microfs::BlockCache cache(1024 * 32, 4);
  auto opened = microfs::Table::Open(env.get(), path, &cache);
  if (!opened.ok()) {
    throw std::runtime_error(opened.status().message());
  }
  MICROFS_CHECK(opened.ok());
  auto table = std::move(opened).value();
  std::string value;
  MICROFS_CHECK(table->Get("S0012", &value).ok());
  MICROFS_CHECK(value == "value-12");
  MICROFS_CHECK(table->Get("S0012", &value).ok());
  MICROFS_CHECK(cache.hits() > 0);
  MICROFS_CHECK(cache.misses() > 0);
  MICROFS_CHECK(table->MayContainPrefix("S0079"));
  MICROFS_CHECK(!table->MayContainPrefix("X0001"));
  MICROFS_CHECK(table->Get("S9999", &value).code() ==
                microfs::StatusCode::kNotFound);

  table.reset();
  std::vector<std::byte> file;
  MICROFS_CHECK(env->ReadFile(path, &file).ok());
  file[0] ^= std::byte{1};
  MICROFS_CHECK(env->WriteFile(path, file).ok());
  MICROFS_CHECK(env->SyncFile(path).ok());
  auto corrupt = microfs::Table::Open(env.get(), path);
  MICROFS_CHECK(!corrupt.ok());
  MICROFS_CHECK(corrupt.status().code() == microfs::StatusCode::kCorruption);

  file[0] ^= std::byte{1};
  file.pop_back();
  MICROFS_CHECK(env->WriteFile(path, file).ok());
  MICROFS_CHECK(env->SyncFile(path).ok());
  auto truncated = microfs::Table::Open(env.get(), path);
  MICROFS_CHECK(!truncated.ok());
  MICROFS_CHECK(truncated.status().code() == microfs::StatusCode::kCorruption);
  MICROFS_CHECK(env->RemoveFile(path).ok());
}

MICROFS_TEST(EmptyTableRoundTrip) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("empty-table");
  std::error_code error;
  std::filesystem::remove(path, error);
  auto created = microfs::TableBuilder::Create(env.get(), path);
  MICROFS_CHECK(created.ok());
  auto builder = std::move(created).value();
  MICROFS_CHECK(builder->Finish().ok());
  auto opened = microfs::Table::Open(env.get(), path);
  MICROFS_CHECK(opened.ok());
  std::string value;
  MICROFS_CHECK(opened.value()->Get("missing", &value).code() ==
                microfs::StatusCode::kNotFound);
  MICROFS_CHECK(env->RemoveFile(path).ok());
}

MICROFS_TEST(VersionSetManifestReplayAndTailRecovery) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("versions");
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::uint64_t table_number = 0;
  {
    auto opened = microfs::VersionSet::Open(env.get(), path);
    MICROFS_CHECK(opened.ok());
    auto versions = std::move(opened).value();
    table_number = versions->NewFileNumber();
    MICROFS_CHECK(table_number == 2);
    microfs::VersionEdit edit;
    edit.last_sequence = 10;
    edit.next_file_number = versions->next_file_number();
    edit.added_files.push_back({table_number, 0, 123, "a", "z"});
    MICROFS_CHECK(versions->LogAndApply(edit).ok());
    MICROFS_CHECK(versions->Files().size() == 1);
    MICROFS_CHECK(versions->last_sequence() == 10);
  }

  const auto manifest_path =
      (std::filesystem::path(path) / "MANIFEST-000001").string();
  const std::array<std::byte, 3> torn{std::byte{1}, std::byte{2},
                                      std::byte{3}};
  MICROFS_CHECK(env->AppendFile(manifest_path, torn).ok());
  {
    auto reopened = microfs::VersionSet::Open(env.get(), path);
    MICROFS_CHECK(reopened.ok());
    auto versions = std::move(reopened).value();
    MICROFS_CHECK(versions->Files().size() == 1);
    MICROFS_CHECK(versions->last_sequence() == 10);
    microfs::VersionEdit edit;
    edit.last_sequence = 11;
    edit.next_file_number = versions->next_file_number();
    edit.deleted_files.emplace_back(0, table_number);
    MICROFS_CHECK(versions->LogAndApply(edit).ok());
  }
  auto final_version = microfs::VersionSet::Open(env.get(), path);
  MICROFS_CHECK(final_version.ok());
  MICROFS_CHECK(final_version.value()->Files().empty());
  MICROFS_CHECK(final_version.value()->last_sequence() == 11);
  final_version.value().reset();
  std::filesystem::remove_all(path, error);
}

MICROFS_TEST(EnvironmentExclusiveFileLock) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("lock");
  std::error_code error;
  std::filesystem::remove(path, error);
  MICROFS_CHECK(env->LockFile(path).ok());
  MICROFS_CHECK(env->LockFile(path).code() == microfs::StatusCode::kBusy);
  MICROFS_CHECK(env->UnlockFile(path).ok());
  MICROFS_CHECK(env->LockFile(path).ok());
  MICROFS_CHECK(env->UnlockFile(path).ok());
  MICROFS_CHECK(env->RemoveFile(path).ok());
}

MICROFS_TEST(DatabaseCrudSnapshotsIteratorsAndRecovery) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("db-crud");
  std::error_code error;
  std::filesystem::remove_all(path, error);
  microfs::DBOptions options;
  options.write_buffer_size = 4096;
  options.l0_compaction_trigger = 3;
  options.l0_write_slowdown_trigger = 6;
  options.l0_stop_writes_trigger = 9;
  std::string value;
  microfs::Snapshot snapshot;
  {
    auto opened = microfs::DB::Open(env.get(), path, options);
    MICROFS_CHECK(opened.ok());
    auto db = std::move(opened).value();
    auto duplicate = microfs::DB::Open(env.get(), path, options);
    MICROFS_CHECK(!duplicate.ok());
    MICROFS_CHECK(duplicate.status().code() == microfs::StatusCode::kBusy);
    MICROFS_CHECK(db->Put("", "empty-key").ok());
    MICROFS_CHECK(db->Put("alpha", "one").ok());
    const std::string_view binary_key("binary\0key", 10);
    MICROFS_CHECK(db->Put(binary_key, "binary").ok());
    auto snapshot_result = db->GetSnapshot();
    MICROFS_CHECK(snapshot_result.ok());
    snapshot = snapshot_result.value();
    MICROFS_CHECK(db->Put("alpha", "two").ok());
    MICROFS_CHECK(db->Delete("").ok());

    MICROFS_CHECK(db->Get("alpha", &value).ok());
    MICROFS_CHECK(value == "two");
    MICROFS_CHECK(db->Get("alpha", &value, &snapshot).ok());
    MICROFS_CHECK(value == "one");
    MICROFS_CHECK(db->Get("", &value).code() ==
                  microfs::StatusCode::kNotFound);
    MICROFS_CHECK(db->Get("", &value, &snapshot).ok());
    MICROFS_CHECK(value == "empty-key");
    MICROFS_CHECK(db->Get("foreign", &value, &snapshot).code() ==
                  microfs::StatusCode::kNotFound);

    microfs::WriteBatch batch;
    MICROFS_CHECK(batch.Put("beta", "B").ok());
    MICROFS_CHECK(batch.Put("gamma", "G").ok());
    MICROFS_CHECK(batch.Delete("alpha").ok());
    MICROFS_CHECK(db->Write(batch).ok());
    MICROFS_CHECK(db->Get("alpha", &value).code() ==
                  microfs::StatusCode::kNotFound);
    MICROFS_CHECK(db->Get("alpha", &value, &snapshot).ok());
    MICROFS_CHECK(value == "one");

    auto iterator_result = db->NewIterator(&snapshot);
    MICROFS_CHECK(iterator_result.ok());
    auto iterator = std::move(iterator_result).value();
    iterator->SeekToFirst();
    MICROFS_CHECK(iterator->Valid());
    MICROFS_CHECK(iterator->key().empty());
    iterator->Next();
    MICROFS_CHECK(iterator->key() == "alpha");
    iterator->SeekToLast();
    MICROFS_CHECK(iterator->key() == binary_key);
    iterator->Prev();
    MICROFS_CHECK(iterator->key() == "alpha");
    MICROFS_CHECK(db->ReleaseSnapshot(snapshot).ok());
    MICROFS_CHECK(db->NewIterator(&snapshot).status().code() ==
                  microfs::StatusCode::kInvalidArgument);
    MICROFS_CHECK(db->Close().ok());
    MICROFS_CHECK(db->Get("alpha", &value).code() ==
                  microfs::StatusCode::kBusy);
  }
  {
    auto reopened = microfs::DB::Open(env.get(), path, options);
    MICROFS_CHECK(reopened.ok());
    auto db = std::move(reopened).value();
    MICROFS_CHECK(db->Get("beta", &value).ok());
    MICROFS_CHECK(value == "B");
    MICROFS_CHECK(db->Get("gamma", &value).ok());
    MICROFS_CHECK(value == "G");
    MICROFS_CHECK(db->Get("alpha", &value).code() ==
                  microfs::StatusCode::kNotFound);
    MICROFS_CHECK(db->Close().ok());
  }
  std::filesystem::remove_all(path, error);
}

MICROFS_TEST(DatabaseAutomaticFlushAndCompaction) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("db-flush");
  std::error_code error;
  std::filesystem::remove_all(path, error);
  microfs::DBOptions options;
  options.write_buffer_size = 256;
  options.arena_block_size = 128;
  options.l0_compaction_trigger = 2;
  options.l0_write_slowdown_trigger = 4;
  options.l0_stop_writes_trigger = 8;
  options.table_options.data_block_size = 128;
  std::string value;
  {
    auto opened = microfs::DB::Open(env.get(), path, options);
    MICROFS_CHECK(opened.ok());
    auto db = std::move(opened).value();
    for (std::size_t i = 0; i < 80; ++i) {
      const std::string key = "key-" + std::to_string(i);
      MICROFS_CHECK(db->Put(key, "value-" + std::to_string(i)).ok());
    }
    MICROFS_CHECK(db->Compact().ok());
    for (std::size_t i = 0; i < 80; ++i) {
      const std::string key = "key-" + std::to_string(i);
      MICROFS_CHECK(db->Get(key, &value).ok());
      MICROFS_CHECK(value == "value-" + std::to_string(i));
    }
    auto l0 = db->GetProperty("microfs.num-files-at-level0");
    auto l1 = db->GetProperty("microfs.num-files-at-level1");
    MICROFS_CHECK(l0.ok() && l0.value() == "0");
    MICROFS_CHECK(l1.ok() && l1.value() == "1");
    MICROFS_CHECK(db->Close().ok());
  }
  {
    auto reopened = microfs::DB::Open(env.get(), path, options);
    MICROFS_CHECK(reopened.ok());
    auto db = std::move(reopened).value();
    for (std::size_t i = 0; i < 80; ++i) {
      const std::string key = "key-" + std::to_string(i);
      MICROFS_CHECK(db->Get(key, &value).ok());
    }
    MICROFS_CHECK(db->Close().ok());
  }
  std::filesystem::remove_all(path, error);
}

MICROFS_TEST(TickStoreBatchedEncodingAndRanges) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("tick-store");
  std::error_code error;
  std::filesystem::remove_all(path, error);
  auto opened = microfs::DB::Open(env.get(), path);
  MICROFS_CHECK(opened.ok());
  auto db = std::move(opened).value();
  MICROFS_CHECK(db->Put("unrelated-user-key", "value").ok());
  microfs::TickStore store(db.get());
  const std::string binary_symbol("A\0B", 3);
  std::vector<microfs::Tick> first_batch{
      {binary_symbol, 100, -500, 7},
      {binary_symbol, 105, -498, 12},
      {binary_symbol, 120, -510, 3},
  };
  MICROFS_CHECK(store.AppendBatch(first_batch).ok());
  MICROFS_CHECK(store.AppendTick({binary_symbol, 130, -509, 9}).ok());
  MICROFS_CHECK(store.AppendBatch(
                    {{"XYZ", 100, 1000000000, 100},
                     {"XYZ", 200, 999999999, 120}})
                    .ok());
  auto range = store.QueryRange(binary_symbol, 104, 120);
  MICROFS_CHECK(range.ok());
  MICROFS_CHECK(range.value().size() == 2);
  MICROFS_CHECK(range.value()[0].timestamp_ns == 105);
  MICROFS_CHECK(range.value()[0].price == -498);
  MICROFS_CHECK(range.value()[0].size == 12);
  MICROFS_CHECK(range.value()[1].timestamp_ns == 120);
  MICROFS_CHECK(range.value()[1].price == -510);
  auto last = store.LastTick(binary_symbol);
  MICROFS_CHECK(last.ok());
  MICROFS_CHECK(last.value().timestamp_ns == 130);
  MICROFS_CHECK(last.value().price == -509);
  MICROFS_CHECK(store.AppendTick({binary_symbol, 130, 1, 1}).code() ==
                microfs::StatusCode::kInvalidArgument);
  MICROFS_CHECK(store.AppendBatch({{"XYZ", 300, 1, 1},
                                  {"ABC", 301, 1, 1}})
                    .code() == microfs::StatusCode::kInvalidArgument);
  auto all = store.ScanAll(100, 200);
  MICROFS_CHECK(all.ok());
  MICROFS_CHECK(all.value().size() == 6);
  MICROFS_CHECK(db->Close().ok());
  std::filesystem::remove_all(path, error);
}

MICROFS_TEST(DatabaseOracleTenThousandOperations) {
  auto env = microfs::NewDefaultEnv();
  const std::string path = TempPath("db-oracle");
  std::error_code error;
  std::filesystem::remove_all(path, error);
  microfs::DBOptions options;
  options.write_buffer_size = 512 * 1024 * 1024;
  options.wal_options.sync_policy = microfs::SyncPolicy::kNever;
  std::unique_ptr<microfs::DB> db;
  auto open_database = [&] {
    auto opened = microfs::DB::Open(env.get(), path, options);
    if (!opened.ok()) throw std::runtime_error(opened.status().message());
    db = std::move(opened).value();
  };
  open_database();

  std::mt19937_64 random(0x6d6963726f667331ULL);
  std::map<std::string, std::string> oracle;
  struct OracleSnapshot {
    microfs::Snapshot snapshot;
    std::map<std::string, std::string> state;
  };
  std::optional<OracleSnapshot> active_snapshot;
  for (std::size_t operation = 0; operation < 10000; ++operation) {
    const std::string key = "k-" + std::to_string(random() % 512);
    const std::uint64_t choice = random() % 100;
    if (choice < 30) {
      const std::string value =
          "v-" + std::to_string(random() % 1000000) + "-" +
          std::to_string(operation);
      MICROFS_CHECK(db->Put(key, value).ok());
      oracle.insert_or_assign(key, value);
    } else if (choice < 42) {
      MICROFS_CHECK(db->Delete(key).ok());
      oracle.erase(key);
    } else if (choice < 78) {
      std::string actual;
      const microfs::Status status = db->Get(key, &actual);
      const auto expected = oracle.find(key);
      if (expected == oracle.end()) {
        MICROFS_CHECK(status.code() == microfs::StatusCode::kNotFound);
      } else {
        MICROFS_CHECK(status.ok());
        MICROFS_CHECK(actual == expected->second);
      }
    } else if (choice < 97) {
      std::string target = "k-" + std::to_string(random() % 512);
      auto iterator_result = db->NewIterator();
      MICROFS_CHECK(iterator_result.ok());
      auto iterator = std::move(iterator_result).value();
      iterator->Seek(target);
      auto expected = oracle.lower_bound(target);
      MICROFS_CHECK(iterator->Valid() == (expected != oracle.end()));
      std::size_t scanned = 0;
      while (iterator->Valid() && scanned < 8) {
        MICROFS_CHECK(expected != oracle.end());
        MICROFS_CHECK(iterator->key() == expected->first);
        MICROFS_CHECK(iterator->value() == expected->second);
        iterator->Next();
        ++expected;
        ++scanned;
      }
    } else if (!active_snapshot.has_value()) {
      auto result = db->GetSnapshot();
      MICROFS_CHECK(result.ok());
      active_snapshot.emplace(
          OracleSnapshot{result.value(), oracle});
    } else {
      const std::string snapshot_key =
          "k-" + std::to_string(random() % 512);
      std::string actual;
      const microfs::Status status =
          db->Get(snapshot_key, &actual, &active_snapshot->snapshot);
      const auto expected = active_snapshot->state.find(snapshot_key);
      if (expected == active_snapshot->state.end()) {
        MICROFS_CHECK(status.code() == microfs::StatusCode::kNotFound);
      } else {
        MICROFS_CHECK(status.ok());
        MICROFS_CHECK(actual == expected->second);
      }
      if (random() % 4 == 0) {
        MICROFS_CHECK(db->ReleaseSnapshot(active_snapshot->snapshot).ok());
        active_snapshot.reset();
      }
    }

    if ((operation + 1) % 2500 == 0 &&
        operation + 1 != 10000) {
      if (active_snapshot.has_value()) {
        MICROFS_CHECK(db->ReleaseSnapshot(active_snapshot->snapshot).ok());
        active_snapshot.reset();
      }
      MICROFS_CHECK(db->Close().ok());
      db.reset();
      open_database();
      for (std::size_t check = 0; check < 512; ++check) {
        const std::string check_key = "k-" + std::to_string(check);
        std::string actual;
        const microfs::Status status = db->Get(check_key, &actual);
        const auto expected = oracle.find(check_key);
        if (expected == oracle.end()) {
          MICROFS_CHECK(status.code() == microfs::StatusCode::kNotFound);
        } else {
          MICROFS_CHECK(status.ok());
          MICROFS_CHECK(actual == expected->second);
        }
      }
    }
  }
  if (active_snapshot.has_value()) {
    MICROFS_CHECK(db->ReleaseSnapshot(active_snapshot->snapshot).ok());
  }
  auto scan_result = db->NewIterator();
  MICROFS_CHECK(scan_result.ok());
  auto scan = std::move(scan_result).value();
  scan->SeekToFirst();
  auto expected = oracle.begin();
  while (scan->Valid() && expected != oracle.end()) {
    MICROFS_CHECK(scan->key() == expected->first);
    MICROFS_CHECK(scan->value() == expected->second);
    scan->Next();
    ++expected;
  }
  MICROFS_CHECK(!scan->Valid());
  MICROFS_CHECK(expected == oracle.end());
  MICROFS_CHECK(db->Close().ok());
  db.reset();
  open_database();
  for (const auto& [key, expected_value] : oracle) {
    std::string actual;
    MICROFS_CHECK(db->Get(key, &actual).ok());
    MICROFS_CHECK(actual == expected_value);
  }
  MICROFS_CHECK(db->Close().ok());
  db.reset();
  std::filesystem::remove_all(path, error);
}

}  // namespace

int main() { return microfs::test::RunAll(); }
