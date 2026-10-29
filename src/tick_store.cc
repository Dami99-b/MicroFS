#include "microfs/tick_store.h"

#include <limits>
#include <new>
#include <utility>

#include "microfs/coding.h"

namespace microfs {
namespace {

constexpr std::string_view kTickNamespace("\x01TICK", 5);

Status EncodeKey(std::string_view symbol, std::uint64_t timestamp,
                 std::string* key) {
  if (key == nullptr) return Status::InvalidArgument("null tick key output");
  try {
    key->assign(kTickNamespace);
    key->reserve(kTickNamespace.size() + symbol.size() + 10);
    for (const char byte : symbol) {
      if (byte == '\0') {
        key->push_back('\0');
        key->push_back(static_cast<char>(0xff));
      } else {
        key->push_back(byte);
      }
    }
    key->push_back('\0');
    key->push_back('\0');
    const std::size_t offset = key->size();
    key->resize(offset + 8);
    PutBigEndian64(key->data() + offset, timestamp);
  } catch (...) {
    return Status::ResourceExhausted("encoding tick key failed");
  }
  return Status::OK();
}

bool DecodeKey(std::string_view key, std::string* symbol,
               std::uint64_t* timestamp) {
  if (symbol == nullptr || timestamp == nullptr) return false;
  if (!key.starts_with(kTickNamespace)) return false;
  try {
    symbol->clear();
    std::size_t cursor = kTickNamespace.size();
    while (cursor + 1 < key.size()) {
      const auto byte = static_cast<std::uint8_t>(key[cursor]);
      if (byte != 0) {
        symbol->push_back(key[cursor++]);
        continue;
      }
      const auto escape = static_cast<std::uint8_t>(key[cursor + 1]);
      if (escape == 0) {
        cursor += 2;
        if (key.size() - cursor != 8) return false;
        *timestamp = DecodeBigEndian64(key.data() + cursor);
        return true;
      }
      if (escape != 0xffU) return false;
      symbol->push_back('\0');
      cursor += 2;
    }
  } catch (...) {
    return false;
  }
  return false;
}

std::uint64_t ZigZagEncode(std::int64_t value) {
  if (value >= 0) return static_cast<std::uint64_t>(value) * 2;
  return (static_cast<std::uint64_t>(-(value + 1)) * 2) + 1;
}

bool ZigZagDecode(std::uint64_t encoded, std::int64_t* value) {
  if (value == nullptr) return false;
  const std::uint64_t magnitude = encoded >> 1U;
  if ((encoded & 1U) == 0) {
    if (magnitude >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    *value = static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude ==
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    *value = std::numeric_limits<std::int64_t>::min();
  } else {
    *value = -static_cast<std::int64_t>(magnitude + 1);
  }
  return true;
}

bool Subtract(std::int64_t current, std::int64_t previous,
              std::int64_t* delta) {
  if (delta == nullptr) return false;
  if (previous < 0 && current >= 0) {
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(current) +
        static_cast<std::uint64_t>(-(previous + 1)) + 1;
    if (magnitude >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    *delta = static_cast<std::int64_t>(magnitude);
  } else if (previous >= 0 && current < 0) {
    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(previous) +
        static_cast<std::uint64_t>(-(current + 1)) + 1;
    if (magnitude >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
            1) {
      return false;
    }
    *delta = magnitude ==
                     static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()) +
                         1
                 ? std::numeric_limits<std::int64_t>::min()
                 : -static_cast<std::int64_t>(magnitude);
  } else {
    *delta = current - previous;
  }
  return true;
}

bool AddDelta(std::int64_t previous, std::int64_t delta,
              std::int64_t* current) {
  if (current == nullptr) return false;
  if (delta > 0 &&
      previous > std::numeric_limits<std::int64_t>::max() - delta) {
    return false;
  }
  if (delta < 0 &&
      previous < std::numeric_limits<std::int64_t>::min() - delta) {
    return false;
  }
  *current = previous + delta;
  return true;
}

Status EncodeBatch(const std::vector<Tick>& ticks, std::string* encoded) {
  if (encoded == nullptr || ticks.empty()) {
    return Status::InvalidArgument("tick batch must not be empty");
  }
  try {
    std::string result;
    PutVarint64(&result, ticks.size());
    std::int64_t previous_price = 0;
    std::uint64_t previous_timestamp = ticks.front().timestamp_ns;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
      const Tick& tick = ticks[i];
      std::int64_t price_delta = tick.price;
      if (i != 0) {
        if (tick.timestamp_ns <= previous_timestamp ||
            !Subtract(tick.price, previous_price, &price_delta)) {
          return Status::InvalidArgument(
              "tick batch timestamps or price deltas are out of range");
        }
        PutVarint64(&result, tick.timestamp_ns - previous_timestamp);
      }
      PutVarint64(&result, ZigZagEncode(price_delta));
      PutVarint64(&result, tick.size);
      previous_price = tick.price;
      previous_timestamp = tick.timestamp_ns;
    }
    *encoded = std::move(result);
  } catch (...) {
    return Status::ResourceExhausted("encoding tick batch failed");
  }
  return Status::OK();
}

Status DecodeBatch(std::string_view encoded, std::string_view symbol,
                   std::uint64_t first_timestamp, std::vector<Tick>* ticks) {
  if (ticks == nullptr) return Status::InvalidArgument("null tick output");
  std::uint64_t count = 0;
  if (!GetVarint64(&encoded, &count) || count == 0 ||
      count > encoded.size() / 2 + 1 ||
      count > ticks->max_size() - ticks->size()) {
    return Status::Corruption("invalid tick batch count");
  }
  try {
    ticks->reserve(ticks->size() + static_cast<std::size_t>(count));
    std::uint64_t timestamp = first_timestamp;
    std::int64_t price = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      if (i != 0) {
        std::uint64_t delta = 0;
        if (!GetVarint64(&encoded, &delta) || delta == 0 ||
            delta > std::numeric_limits<std::uint64_t>::max() - timestamp) {
          return Status::Corruption("invalid tick timestamp delta");
        }
        timestamp += delta;
      }
      std::uint64_t encoded_price = 0;
      std::uint64_t size = 0;
      std::int64_t price_delta = 0;
      if (!GetVarint64(&encoded, &encoded_price) ||
          !ZigZagDecode(encoded_price, &price_delta) ||
          !GetVarint64(&encoded, &size)) {
        return Status::Corruption("invalid tick price or size");
      }
      if (i == 0) {
        price = price_delta;
      } else if (!AddDelta(price, price_delta, &price)) {
        return Status::Corruption("tick price delta overflows");
      }
      ticks->push_back({std::string(symbol), timestamp, price, size});
    }
  } catch (...) {
    return Status::ResourceExhausted("decoding tick batch failed");
  }
  if (!encoded.empty()) return Status::Corruption("tick batch has trailing bytes");
  return Status::OK();
}

}  // namespace

Status TickStore::AppendTick(const Tick& tick) {
  try {
    return AppendBatch(std::vector<Tick>{tick});
  } catch (...) {
    return Status::ResourceExhausted("creating single-tick batch failed");
  }
}

Status TickStore::AppendBatch(const std::vector<Tick>& ticks) {
  if (db_ == nullptr || ticks.empty()) {
    return Status::InvalidArgument("tick store requires a DB and non-empty batch");
  }
  for (std::size_t i = 0; i < ticks.size(); ++i) {
    if (ticks[i].symbol != ticks.front().symbol ||
        (i != 0 && ticks[i].timestamp_ns <= ticks[i - 1].timestamp_ns)) {
      return Status::InvalidArgument(
          "tick batch must contain ordered ticks for one symbol");
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto last = LastTickLocked(ticks.front().symbol);
  if (last.ok() && ticks.front().timestamp_ns <= last.value().timestamp_ns) {
    return Status::InvalidArgument("tick timestamp is not newer than stored data");
  }
  if (!last.ok() && last.status().code() != StatusCode::kNotFound) {
    return last.status();
  }
  std::string key;
  Status status =
      EncodeKey(ticks.front().symbol, ticks.front().timestamp_ns, &key);
  if (!status.ok()) return status;
  std::string value;
  status = EncodeBatch(ticks, &value);
  if (!status.ok()) return status;
  WriteBatch batch;
  status = batch.Put(key, value);
  if (!status.ok()) return status;
  return db_->Write(batch);
}

StatusOr<std::vector<Tick>> TickStore::QueryRange(
    std::string_view symbol, std::uint64_t from_ns,
    std::uint64_t to_ns) const {
  if (db_ == nullptr || from_ns > to_ns) {
    return Status::InvalidArgument("invalid tick range");
  }
  auto iterator_result = db_->NewIterator();
  if (!iterator_result.ok()) return iterator_result.status();
  auto iterator = std::move(iterator_result).value();
  std::string prefix;
  Status status = EncodeKey(symbol, 0, &prefix);
  if (!status.ok()) return status;
  prefix.resize(prefix.size() - 8);
  iterator->Seek(prefix);
  std::vector<Tick> result;
  while (iterator->Valid()) {
    if (!iterator->key().starts_with(prefix)) break;
    std::string found_symbol;
    std::uint64_t timestamp = 0;
    if (!DecodeKey(iterator->key(), &found_symbol, &timestamp)) {
      return Status::Corruption("invalid encoded tick key");
    }
    if (found_symbol != symbol) break;
    std::vector<Tick> decoded;
    status = DecodeBatch(iterator->value(), found_symbol, timestamp, &decoded);
    if (!status.ok()) return status;
    for (Tick& tick : decoded) {
      if (tick.timestamp_ns >= from_ns && tick.timestamp_ns <= to_ns) {
        result.push_back(std::move(tick));
      }
      if (tick.timestamp_ns > to_ns) break;
    }
    if (!decoded.empty() && decoded.back().timestamp_ns > to_ns) break;
    iterator->Next();
  }
  return result;
}

StatusOr<Tick> TickStore::LastTickLocked(std::string_view symbol) const {
  if (db_ == nullptr) return Status::InvalidArgument("tick store has no DB");
  auto iterator_result = db_->NewIterator();
  if (!iterator_result.ok()) return iterator_result.status();
  auto iterator = std::move(iterator_result).value();
  std::string prefix;
  Status status = EncodeKey(symbol, 0, &prefix);
  if (!status.ok()) return status;
  prefix.resize(prefix.size() - 8);
  iterator->Seek(prefix);
  Tick last;
  bool found = false;
  while (iterator->Valid()) {
    if (!iterator->key().starts_with(prefix)) break;
    std::string found_symbol;
    std::uint64_t timestamp = 0;
    if (!DecodeKey(iterator->key(), &found_symbol, &timestamp)) {
      return Status::Corruption("invalid encoded tick key");
    }
    if (found_symbol != symbol) break;
    std::vector<Tick> decoded;
    status = DecodeBatch(iterator->value(), found_symbol, timestamp, &decoded);
    if (!status.ok()) return status;
    if (!decoded.empty()) {
      last = std::move(decoded.back());
      found = true;
    }
    iterator->Next();
  }
  if (!found) return Status::NotFound("symbol has no ticks");
  return last;
}

StatusOr<Tick> TickStore::LastTick(std::string_view symbol) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return LastTickLocked(symbol);
}

StatusOr<std::vector<Tick>> TickStore::ScanAll(std::uint64_t from_ns,
                                               std::uint64_t to_ns) const {
  if (db_ == nullptr || from_ns > to_ns) {
    return Status::InvalidArgument("invalid multi-symbol tick range");
  }
  auto iterator_result = db_->NewIterator();
  if (!iterator_result.ok()) return iterator_result.status();
  auto iterator = std::move(iterator_result).value();
  iterator->SeekToFirst();
  std::vector<Tick> result;
  while (iterator->Valid()) {
    if (!iterator->key().starts_with(kTickNamespace)) {
      iterator->Next();
      continue;
    }
    std::string symbol;
    std::uint64_t timestamp = 0;
    if (!DecodeKey(iterator->key(), &symbol, &timestamp)) {
      return Status::Corruption("invalid encoded tick key");
    }
    std::vector<Tick> decoded;
    Status status = DecodeBatch(iterator->value(), symbol, timestamp, &decoded);
    if (!status.ok()) return status;
    for (Tick& tick : decoded) {
      if (tick.timestamp_ns >= from_ns && tick.timestamp_ns <= to_ns) {
        result.push_back(std::move(tick));
      }
    }
    iterator->Next();
  }
  return result;
}

}  // namespace microfs
