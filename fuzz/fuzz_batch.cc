#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "microfs/db.h"
#include "microfs/env.h"
#include "microfs/wal.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  static std::atomic<std::uint64_t> next_id{0};
  auto env = microfs::NewDefaultEnv();
  const auto path =
      std::filesystem::temp_directory_path() /
      ("microfs-fuzz-batch-" + std::to_string(next_id.fetch_add(1)));
  std::error_code error;
  std::filesystem::remove_all(path, error);
  auto opened = microfs::DB::Open(env.get(), path.string());
  if (opened.ok()) {
    static_cast<void>(opened.value()->Close());
    const std::string log_path = (path / "LOG-000001").string();
    static_cast<void>(env->RemoveFile(log_path));
    auto wal = microfs::WAL::Open(env.get(), log_path);
    if (wal.ok()) {
      const std::string_view input(reinterpret_cast<const char*>(data), size);
      static_cast<void>(wal.value()->Append(input));
      wal.value().reset();
      auto replayed = microfs::DB::Open(env.get(), path.string());
      if (replayed.ok()) static_cast<void>(replayed.value()->Close());
    }
  }
  std::filesystem::remove_all(path, error);
  return 0;
}
