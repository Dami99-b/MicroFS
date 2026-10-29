#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace microfs {

class Arena {
 public:
  explicit Arena(std::size_t block_size = 4096) : block_size_(block_size) {}
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena() {
    for (const Block& block : blocks_) delete[] block.data;
  }

  void* Allocate(std::size_t bytes, std::size_t alignment = alignof(void*)) {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
      return nullptr;
    }
    if (!blocks_.empty()) {
      Block& current = blocks_.back();
      const auto address =
          reinterpret_cast<std::uintptr_t>(current.data + current.used);
      if (address >
          std::numeric_limits<std::uintptr_t>::max() - (alignment - 1)) {
        return nullptr;
      }
      const auto aligned =
          (address + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
      const std::size_t offset =
          static_cast<std::size_t>(aligned -
                                   reinterpret_cast<std::uintptr_t>(current.data));
      if (offset <= current.size && bytes <= current.size - offset) {
        current.used = offset + bytes;
        return current.data + offset;
      }
    }
    if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
      return nullptr;
    }
    const std::size_t size = std::max(block_size_, bytes + alignment - 1);
    auto* data = new (std::nothrow) std::byte[size];
    if (data == nullptr) return nullptr;
    try {
      blocks_.push_back({data, size, 0});
    } catch (...) {
      delete[] data;
      return nullptr;
    }
    allocated_bytes_ += size;
    return Allocate(bytes, alignment);
  }

  [[nodiscard]] std::size_t allocated_bytes() const {
    return allocated_bytes_;
  }

 private:
  struct Block {
    std::byte* data;
    std::size_t size;
    std::size_t used;
  };

  std::size_t block_size_;
  std::size_t allocated_bytes_ = 0;
  std::vector<Block> blocks_;
};

}  // namespace microfs
