#include "microfs/arena.h"
#include "microfs/coding.h"
#include "microfs/crc32c.h"
#include "microfs/env.h"
#include "microfs/hash.h"
#include "microfs/histogram.h"
#include "microfs/memtable.h"
#include "microfs/skiplist.h"
#include "microfs/status.h"
#include "test_harness.h"
#include "microfs/wal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
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

}  // namespace

int main() { return microfs::test::RunAll(); }
