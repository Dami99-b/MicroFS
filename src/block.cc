#include "microfs/block.h"

#include <algorithm>
#include <limits>

#include "microfs/coding.h"
#include "microfs/skiplist.h"

namespace microfs {
namespace {

std::size_t SharedPrefix(std::string_view lhs, std::string_view rhs) {
  const std::size_t limit = std::min(lhs.size(), rhs.size());
  std::size_t shared = 0;
  while (shared < limit && lhs[shared] == rhs[shared]) ++shared;
  return shared;
}

}  // namespace

BlockBuilder::BlockBuilder(std::size_t restart_interval)
    : restart_interval_(restart_interval) {
  restarts_.push_back(0);
}

Status BlockBuilder::Add(std::string_view key, std::string_view value) {
  if (restart_interval_ == 0) {
    return Status::InvalidArgument("restart interval must be nonzero");
  }
  if (has_last_key_ &&
      LexicographicComparator()(last_key_, key) >= 0) {
    return Status::InvalidArgument("block keys must be strictly increasing");
  }
  std::size_t shared = 0;
  if (entries_since_restart_ >= restart_interval_) {
    if (data_.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status::ResourceExhausted("block exceeds 32-bit restart offsets");
    }
    try {
      restarts_.push_back(static_cast<std::uint32_t>(data_.size()));
    } catch (...) {
      return Status::ResourceExhausted("allocating block restart point");
    }
    entries_since_restart_ = 0;
  } else {
    shared = SharedPrefix(last_key_, key);
  }
  try {
    PutVarint64(&data_, shared);
    PutVarint64(&data_, key.size() - shared);
    PutVarint64(&data_, value.size());
    data_.append(key.substr(shared));
    data_.append(value);
    last_key_.assign(key);
    has_last_key_ = true;
    ++entries_since_restart_;
  } catch (...) {
    return Status::ResourceExhausted("building data block exhausted memory");
  }
  return Status::OK();
}

Status BlockBuilder::Finish(std::string* output) const {
  if (output == nullptr) return Status::InvalidArgument("null block output");
  if (data_.size() > std::numeric_limits<std::uint32_t>::max() ||
      restarts_.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::ResourceExhausted("block exceeds 32-bit format limits");
  }
  try {
    std::string result = data_;
    for (const std::uint32_t restart : restarts_) PutFixed32(&result, restart);
    PutFixed32(&result, static_cast<std::uint32_t>(restarts_.size()));
    *output = std::move(result);
  } catch (...) {
    return Status::ResourceExhausted("finishing data block exhausted memory");
  }
  return Status::OK();
}

void BlockBuilder::Reset() {
  entries_since_restart_ = 0;
  has_last_key_ = false;
  last_key_.clear();
  data_.clear();
  restarts_.clear();
  restarts_.push_back(0);
}

std::size_t BlockBuilder::CurrentSizeEstimate() const {
  return data_.size() + restarts_.size() * sizeof(std::uint32_t) +
         sizeof(std::uint32_t);
}

Status BlockReader::Open(std::string_view bytes) {
  SetInvalid();
  restart_offsets_.clear();
  if (bytes.size() < 8) return Status::Corruption("block is shorter than footer");
  const std::uint32_t restart_count =
      DecodeFixed32(bytes.data() + bytes.size() - 4);
  if (restart_count == 0 ||
      restart_count > (bytes.size() - 4) / sizeof(std::uint32_t)) {
    return Status::Corruption("invalid block restart count");
  }
  const std::size_t restart_bytes =
      static_cast<std::size_t>(restart_count) * sizeof(std::uint32_t);
  const std::size_t entries_end = bytes.size() - restart_bytes - 4;
  try {
    restart_offsets_.reserve(restart_count);
    for (std::uint32_t i = 0; i < restart_count; ++i) {
      const std::size_t offset = entries_end + i * sizeof(std::uint32_t);
      const std::uint32_t restart = DecodeFixed32(bytes.data() + offset);
      if (restart > entries_end ||
          (i == 0 && restart != 0) ||
          (i != 0 && restart <= restart_offsets_.back())) {
        return Status::Corruption("invalid block restart offset");
      }
      restart_offsets_.push_back(restart);
    }
  } catch (...) {
    restart_offsets_.clear();
    return Status::ResourceExhausted("opening block exhausted memory");
  }

  bytes_ = bytes;
  entries_end_ = entries_end;
  if (entries_end_ == 0) {
    if (restart_offsets_.size() != 1 || restart_offsets_[0] != 0) {
      SetInvalid();
      return Status::Corruption("invalid empty block restart array");
    }
    status_ = Status::OK();
    return status_;
  }
  std::size_t cursor = 0;
  std::size_t restart_index = 0;
  std::string previous_key;
  bool has_previous_key = false;
  while (cursor < entries_end_) {
    const bool is_restart =
        restart_index < restart_offsets_.size() &&
        restart_offsets_[restart_index] == cursor;
    if (restart_index < restart_offsets_.size() &&
        restart_offsets_[restart_index] < cursor) {
      SetInvalid();
      return Status::Corruption("restart offset is not an entry boundary");
    }
    std::size_t next = 0;
    std::string current_key;
    std::string_view current_value;
    Status status =
        DecodeEntry(cursor, previous_key, &next, &current_key, &current_value);
    if (!status.ok()) {
      SetInvalid();
      return status;
    }
    if (is_restart) {
      const char* entry = bytes_.data() + cursor;
      std::string_view encoded(entry, entries_end_ - cursor);
      std::uint64_t shared = 0;
      if (!GetVarint64(&encoded, &shared) || shared != 0) {
        SetInvalid();
        return Status::Corruption("restart entry uses prefix compression");
      }
      ++restart_index;
    }
    if (has_previous_key &&
        LexicographicComparator()(previous_key, current_key) >= 0) {
      SetInvalid();
      return Status::Corruption("block keys are not strictly increasing");
    }
    cursor = next;
    previous_key = std::move(current_key);
    has_previous_key = true;
  }
  if (cursor != entries_end_ || restart_index != restart_offsets_.size()) {
    SetInvalid();
    return Status::Corruption("block restart array does not match entries");
  }
  status_ = Status::OK();
  return status_;
}

Status BlockReader::DecodeEntry(std::size_t offset,
                                std::string_view previous_key,
                                std::size_t* next_offset, std::string* key,
                                std::string_view* value) const {
  if (next_offset == nullptr || key == nullptr || value == nullptr ||
      offset >= entries_end_) {
    return Status::Corruption("invalid block entry offset");
  }
  std::string_view encoded(bytes_.data() + offset, entries_end_ - offset);
  std::uint64_t shared = 0;
  std::uint64_t non_shared = 0;
  std::uint64_t value_size = 0;
  if (!GetVarint64(&encoded, &shared) ||
      !GetVarint64(&encoded, &non_shared) ||
      !GetVarint64(&encoded, &value_size) || shared > previous_key.size() ||
      non_shared > encoded.size() ||
      value_size > encoded.size() - static_cast<std::size_t>(non_shared)) {
    return Status::Corruption("invalid block entry lengths");
  }
  try {
    key->assign(previous_key.substr(0, static_cast<std::size_t>(shared)));
    key->append(encoded.substr(0, static_cast<std::size_t>(non_shared)));
  } catch (...) {
    return Status::ResourceExhausted("decoding block key exhausted memory");
  }
  encoded.remove_prefix(static_cast<std::size_t>(non_shared));
  *value = encoded.substr(0, static_cast<std::size_t>(value_size));
  const std::size_t consumed = offset + (entries_end_ - offset - encoded.size()) +
                               static_cast<std::size_t>(value_size);
  if (consumed > entries_end_) {
    return Status::Corruption("block entry extends past its end");
  }
  *next_offset = consumed;
  return Status::OK();
}

void BlockReader::SetInvalid() {
  valid_ = false;
  entry_offset_ = 0;
  next_offset_ = 0;
  key_.clear();
  value_ = {};
}

void BlockReader::SeekToFirst() {
  if (!status_.ok() || entries_end_ == 0) {
    SetInvalid();
    return;
  }
  std::size_t next = 0;
  std::string key;
  std::string_view value;
  Status status = DecodeEntry(0, {}, &next, &key, &value);
  if (!status.ok()) {
    status_ = status;
    SetInvalid();
    return;
  }
  entry_offset_ = 0;
  next_offset_ = next;
  key_ = std::move(key);
  value_ = value;
  valid_ = true;
}

void BlockReader::SeekToLast() {
  SeekToFirst();
  while (valid_ && next_offset_ < entries_end_) Next();
}

void BlockReader::Seek(std::string_view target) {
  SeekToFirst();
  while (valid_ && LexicographicComparator()(key_, target) < 0) Next();
}

void BlockReader::Next() {
  if (!valid_) return;
  if (next_offset_ >= entries_end_) {
    SetInvalid();
    return;
  }
  std::size_t next = 0;
  std::string key;
  std::string_view value;
  Status status = DecodeEntry(next_offset_, key_, &next, &key, &value);
  if (!status.ok()) {
    status_ = status;
    SetInvalid();
    return;
  }
  entry_offset_ = next_offset_;
  next_offset_ = next;
  key_ = std::move(key);
  value_ = value;
}

void BlockReader::Prev() {
  if (!valid_ || entry_offset_ == 0) {
    SetInvalid();
    return;
  }
  std::size_t cursor = 0;
  std::string preceding_key;
  std::string_view preceding_value;
  std::size_t preceding_offset = 0;
  std::size_t preceding_next = 0;
  std::string previous_key;
  bool found = false;
  while (cursor < entry_offset_) {
    std::size_t next = 0;
    std::string key;
    std::string_view value;
    Status status = DecodeEntry(cursor, previous_key, &next, &key, &value);
    if (!status.ok() || next <= cursor || next > entry_offset_) {
      status_ = status.ok() ? Status::Corruption("invalid previous block entry")
                            : status;
      SetInvalid();
      return;
    }
    preceding_offset = cursor;
    preceding_next = next;
    preceding_key = std::move(key);
    preceding_value = value;
    previous_key = preceding_key;
    cursor = next;
    found = true;
  }
  if (!found || cursor != entry_offset_) {
    status_ = Status::Corruption("block iterator position is not an entry");
    SetInvalid();
    return;
  }
  entry_offset_ = preceding_offset;
  next_offset_ = preceding_next;
  key_ = std::move(preceding_key);
  value_ = preceding_value;
}

}  // namespace microfs
