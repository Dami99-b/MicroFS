#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "microfs/status.h"

namespace microfs {

class BlockBuilder {
 public:
  explicit BlockBuilder(std::size_t restart_interval = 16);

  Status Add(std::string_view key, std::string_view value);
  Status Finish(std::string* output) const;
  void Reset();
  [[nodiscard]] std::size_t CurrentSizeEstimate() const;

 private:
  std::size_t restart_interval_;
  std::size_t entries_since_restart_ = 0;
  bool has_last_key_ = false;
  std::string last_key_;
  std::string data_;
  std::vector<std::uint32_t> restarts_;
};

class BlockReader {
 public:
  Status Open(std::string_view bytes);

  [[nodiscard]] bool Valid() const { return valid_; }
  [[nodiscard]] const Status& status() const { return status_; }
  [[nodiscard]] std::string_view key() const {
    return valid_ ? std::string_view(key_) : std::string_view();
  }
  [[nodiscard]] std::string_view value() const {
    return valid_ ? value_ : std::string_view();
  }

  void SeekToFirst();
  void SeekToLast();
  void Seek(std::string_view target);
  void Next();
  void Prev();

 private:
  Status DecodeEntry(std::size_t offset, std::string_view previous_key,
                     std::size_t* next_offset, std::string* key,
                     std::string_view* value) const;
  void SetInvalid();

  std::string_view bytes_;
  std::size_t entries_end_ = 0;
  std::vector<std::uint32_t> restart_offsets_;
  std::size_t entry_offset_ = 0;
  std::size_t next_offset_ = 0;
  std::string key_;
  std::string_view value_;
  bool valid_ = false;
  Status status_;
};

}  // namespace microfs
