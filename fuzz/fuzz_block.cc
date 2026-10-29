#include <cstddef>
#include <cstdint>
#include <string_view>

#include "microfs/block.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  microfs::BlockReader reader;
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const microfs::Status status = reader.Open(bytes);
  if (status.ok()) {
    reader.SeekToFirst();
    while (reader.Valid()) reader.Next();
    reader.SeekToLast();
    if (reader.Valid()) reader.Prev();
  }
  return 0;
}
