#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "microfs/db.h"

namespace microfs {

struct Tick {
  std::string symbol;
  std::uint64_t timestamp_ns = 0;
  std::int64_t price = 0;
  std::uint64_t size = 0;
};

class TickStore {
 public:
  explicit TickStore(DB* db) : db_(db) {}

  Status AppendTick(const Tick& tick);
  Status AppendBatch(const std::vector<Tick>& ticks);
  StatusOr<std::vector<Tick>> QueryRange(std::string_view symbol,
                                         std::uint64_t from_ns,
                                         std::uint64_t to_ns) const;
  StatusOr<Tick> LastTick(std::string_view symbol) const;
  StatusOr<std::vector<Tick>> ScanAll(std::uint64_t from_ns,
                                      std::uint64_t to_ns) const;

 private:
  StatusOr<Tick> LastTickLocked(std::string_view symbol) const;

  DB* db_;
  mutable std::mutex mutex_;
};

}  // namespace microfs
