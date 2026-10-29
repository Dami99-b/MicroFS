#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "microfs/env.h"
#include "microfs/table.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  static std::atomic<std::uint64_t> next_id{0};
  auto env = microfs::NewDefaultEnv();
  const auto path =
      std::filesystem::temp_directory_path() /
      ("microfs-fuzz-table-" + std::to_string(next_id.fetch_add(1)));
  const auto* bytes = reinterpret_cast<const std::byte*>(data);
  static_cast<void>(env->WriteFile(path.string(), {bytes, size}));
  static_cast<void>(microfs::Table::Open(env.get(), path.string()));
  std::error_code error;
  std::filesystem::remove(path, error);
  return 0;
}
