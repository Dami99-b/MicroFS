#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "microfs/db.h"
#include "microfs/tick_store.h"

namespace {

bool ParseU64(std::string_view text, std::uint64_t* value) {
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ParseI64(std::string_view text, std::int64_t* value) {
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

int Usage() {
  std::cerr << "usage: microfs_cli DB put KEY VALUE | get KEY | del KEY | "
               "scan | stats | tick SYMBOL NS PRICE SIZE | "
               "ticks SYMBOL FROM_NS TO_NS | last SYMBOL\n";
  return 2;
}

int Fail(const microfs::Status& status) {
  std::cerr << status.message() << '\n';
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  auto env = microfs::NewDefaultEnv();
  auto opened = microfs::DB::Open(env.get(), argv[1]);
  if (!opened.ok()) return Fail(opened.status());
  auto db = std::move(opened).value();
  const std::string_view command(argv[2]);
  microfs::Status status;
  if (command == "put" && argc == 5) {
    status = db->Put(argv[3], argv[4]);
  } else if (command == "get" && argc == 4) {
    std::string value;
    status = db->Get(argv[3], &value);
    if (status.ok()) std::cout << value << '\n';
  } else if (command == "del" && argc == 4) {
    status = db->Delete(argv[3]);
  } else if (command == "scan" && argc == 3) {
    auto iterator_result = db->NewIterator();
    if (!iterator_result.ok()) return Fail(iterator_result.status());
    auto iterator = std::move(iterator_result).value();
    iterator->SeekToFirst();
    while (iterator->Valid()) {
      std::cout << iterator->key() << '\t' << iterator->value() << '\n';
      iterator->Next();
    }
  } else if (command == "stats" && argc == 3) {
    auto property = db->GetProperty("microfs.stats");
    if (!property.ok()) return Fail(property.status());
    std::cout << property.value() << '\n';
  } else if (command == "tick" && argc == 7) {
    std::uint64_t timestamp = 0;
    std::int64_t price = 0;
    std::uint64_t size = 0;
    if (!ParseU64(argv[4], &timestamp) || !ParseI64(argv[5], &price) ||
        !ParseU64(argv[6], &size)) {
      return Usage();
    }
    microfs::TickStore store(db.get());
    status = store.AppendTick({argv[3], timestamp, price, size});
  } else if (command == "ticks" && argc == 6) {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    if (!ParseU64(argv[4], &from) || !ParseU64(argv[5], &to)) {
      return Usage();
    }
    microfs::TickStore store(db.get());
    auto ticks = store.QueryRange(argv[3], from, to);
    if (!ticks.ok()) return Fail(ticks.status());
    for (const auto& tick : ticks.value()) {
      std::cout << tick.symbol << '\t' << tick.timestamp_ns << '\t'
                << tick.price << '\t' << tick.size << '\n';
    }
  } else if (command == "last" && argc == 4) {
    microfs::TickStore store(db.get());
    auto tick = store.LastTick(argv[3]);
    if (!tick.ok()) return Fail(tick.status());
    std::cout << tick.value().symbol << '\t' << tick.value().timestamp_ns
              << '\t' << tick.value().price << '\t' << tick.value().size
              << '\n';
  } else {
    return Usage();
  }
  if (!status.ok()) return Fail(status);
  const microfs::Status close_status = db->Close();
  if (!close_status.ok()) return Fail(close_status);
  return 0;
}
