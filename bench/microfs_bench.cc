#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "microfs/db.h"
#include "microfs/histogram.h"

namespace {

struct Result {
  std::string workload;
  std::size_t repeat;
  std::uint64_t operations;
  double operations_per_second;
  double bytes_per_second;
  std::uint64_t p50_ns;
  std::uint64_t p90_ns;
  std::uint64_t p99_ns;
  std::uint64_t p999_ns;
  std::uint64_t max_ns;
  std::string stats;
};

std::string Key(std::size_t number) {
  std::ostringstream result;
  result << "key-" << std::setw(10) << std::setfill('0') << number;
  return result.str();
}

bool Run(const std::string& path, std::size_t operations, std::size_t repeat,
         std::vector<Result>* results) {
  auto env = microfs::NewDefaultEnv();
  microfs::DBOptions options;
  options.write_buffer_size = 16 * 1024 * 1024;
  options.wal_options.sync_policy = microfs::SyncPolicy::kNever;
  auto opened = microfs::DB::Open(env.get(), path, options);
  if (!opened.ok()) {
    std::cerr << "open: " << opened.status().message() << '\n';
    return false;
  }
  auto db = std::move(opened).value();
  std::vector<std::string> keys;
  try {
    keys.reserve(operations);
    for (std::size_t i = 0; i < operations; ++i) keys.push_back(Key(i));
  } catch (...) {
    std::cerr << "allocating benchmark keys failed\n";
    return false;
  }
  std::mt19937_64 random(0x6d6963726f66735ULL + repeat);
  std::shuffle(keys.begin(), keys.end(), random);
  microfs::Histogram writes;
  std::uint64_t write_bytes = 0;
  auto write_start = std::chrono::steady_clock::now();
  for (const std::string& key : keys) {
    const std::string value = "value-" + key;
    const auto start = std::chrono::steady_clock::now();
    const microfs::Status status = db->Put(key, value);
    const auto end = std::chrono::steady_clock::now();
    if (!status.ok()) {
      std::cerr << "put: " << status.message() << '\n';
      return false;
    }
    writes.Record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count()));
    write_bytes += key.size() + value.size();
  }
  const auto write_end = std::chrono::steady_clock::now();
  const double write_seconds =
      std::chrono::duration<double>(write_end - write_start).count();
  results->push_back(
      {"fillrandom", repeat, operations, operations / write_seconds,
       write_bytes / write_seconds, writes.Percentile(50), writes.Percentile(90),
       writes.Percentile(99), writes.Percentile(99.9), writes.max(), {}});

  microfs::Histogram reads;
  std::uint64_t read_bytes = 0;
  const auto read_start = std::chrono::steady_clock::now();
  for (const std::string& key : keys) {
    std::string value;
    const auto start = std::chrono::steady_clock::now();
    const microfs::Status status = db->Get(key, &value);
    const auto end = std::chrono::steady_clock::now();
    if (!status.ok() || value != "value-" + key) {
      std::cerr << "read verification failed: " << key << '\n';
      return false;
    }
    reads.Record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count()));
    read_bytes += key.size() + value.size();
  }
  const auto read_end = std::chrono::steady_clock::now();
  const double read_seconds =
      std::chrono::duration<double>(read_end - read_start).count();
  results->push_back(
      {"readrandom-hit", repeat, operations, operations / read_seconds,
       read_bytes / read_seconds, reads.Percentile(50), reads.Percentile(90),
       reads.Percentile(99), reads.Percentile(99.9), reads.max(), {}});
  auto stats = db->GetProperty("microfs.stats");
  if (stats.ok()) {
    results->back().stats = stats.value();
    (*results)[results->size() - 2].stats = stats.value();
  }
  const microfs::Status close_status = db->Close();
  if (!close_status.ok()) {
    std::cerr << "close: " << close_status.message() << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t operations = 10000;
  if (argc > 1) {
    const std::string_view text(argv[1]);
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), operations);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        operations == 0) {
      std::cerr << "usage: microfs_bench [operations-per-workload] [output-dir]\n";
      return 2;
    }
  }
  const std::filesystem::path output =
      argc > 2 ? std::filesystem::path(argv[2])
               : std::filesystem::path("bench-results");
  std::error_code error;
  std::filesystem::create_directories(output, error);
  if (error) {
    std::cerr << "creating output directory failed: " << error.message() << '\n';
    return 1;
  }
  std::vector<Result> results;
  const auto warmup = output / "warmup";
  std::filesystem::remove_all(warmup, error);
  if (error ||
      !Run(warmup.string(), std::min<std::size_t>(operations, 1000), 0,
           &results)) {
    return 1;
  }
  std::filesystem::remove_all(warmup, error);
  if (error) {
    std::cerr << "removing warmup database failed: " << error.message() << '\n';
    return 1;
  }
  results.clear();
  for (std::size_t repeat = 1; repeat <= 5; ++repeat) {
    const auto path = output / ("run-" + std::to_string(repeat));
    std::filesystem::remove_all(path, error);
    if (error) {
      std::cerr << "removing prior run failed: " << error.message() << '\n';
      return 1;
    }
    if (!Run(path.string(), operations, repeat, &results)) return 1;
    std::filesystem::remove_all(path, error);
    if (error) {
      std::cerr << "removing measured database failed: " << error.message()
                << '\n';
      return 1;
    }
  }
  std::ofstream json(output / "results.json", std::ios::trunc);
  std::ofstream csv(output / "results.csv", std::ios::trunc);
  if (!json || !csv) {
    std::cerr << "creating benchmark result files failed\n";
    return 1;
  }
  json << "{\n  \"workloads\": [\n";
  csv << "workload,repeat,operations,ops_per_second,bytes_per_second,"
         "p50_ns,p90_ns,p99_ns,p999_ns,max_ns,stats\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const Result& result = results[i];
    if (i != 0) json << ",\n";
    json << "    {\"workload\":\"" << result.workload << "\",\"repeat\":"
         << result.repeat << ",\"operations\":" << result.operations
         << ",\"ops_per_second\":" << std::setprecision(12)
         << result.operations_per_second << ",\"bytes_per_second\":"
         << result.bytes_per_second << ",\"p50_ns\":" << result.p50_ns
         << ",\"p90_ns\":" << result.p90_ns << ",\"p99_ns\":"
         << result.p99_ns << ",\"p999_ns\":" << result.p999_ns
         << ",\"max_ns\":" << result.max_ns << ",\"stats\":\""
         << result.stats << "\"}";
    csv << result.workload << ',' << result.repeat << ',' << result.operations
        << ',' << std::setprecision(12) << result.operations_per_second << ','
        << result.bytes_per_second << ',' << result.p50_ns << ','
        << result.p90_ns << ',' << result.p99_ns << ',' << result.p999_ns
        << ',' << result.max_ns << ",\"" << result.stats << "\"\n";
  }
  json << "\n  ]\n}\n";
  json.close();
  csv.close();
  if (!json || !csv) {
    std::cerr << "writing benchmark results failed\n";
    return 1;
  }
  std::cout << "Wrote five measured repetitions (plus one warmup) to "
            << output.string() << '\n';
  return 0;
}
