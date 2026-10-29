#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "microfs/env.h"
#include "microfs/wal.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  static std::atomic<std::uint64_t> next_id{0};
  auto env = microfs::NewDefaultEnv();
  const auto path =
      std::filesystem::temp_directory_path() /
      ("microfs-fuzz-wal-" + std::to_string(next_id.fetch_add(1)));
  std::vector<std::string> records;
  static_cast<void>(microfs::WAL::Recover(env.get(), path.string(), &records));
  auto wal = microfs::WAL::Open(env.get(), path.string());
  if (wal.ok()) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    static_cast<void>(wal.value()->Append(input));
    wal.value().reset();
    records.clear();
    static_cast<void>(
        microfs::WAL::Recover(env.get(), path.string(), &records));
  }
  std::error_code error;
  std::filesystem::remove(path, error);
  return 0;
}
