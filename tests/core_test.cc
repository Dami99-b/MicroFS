#include "microfs/arena.h"
#include "microfs/coding.h"
#include "microfs/crc32c.h"
#include "microfs/env.h"
#include "microfs/hash.h"
#include "microfs/histogram.h"
#include "microfs/status.h"
#include "test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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

}  // namespace

int main() { return microfs::test::RunAll(); }
