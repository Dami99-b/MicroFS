#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "microfs/env.h"
#include "microfs/status.h"
#include "microfs/wal.h"

namespace microfs {

constexpr std::size_t kNumLevels = 7;

struct FileMeta {
  std::uint64_t number = 0;
  std::uint32_t level = 0;
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
};

struct VersionEdit {
  std::optional<std::uint64_t> last_sequence;
  std::optional<std::uint64_t> next_file_number;
  std::vector<FileMeta> added_files;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> deleted_files;
};

class VersionSet {
 public:
  static StatusOr<std::unique_ptr<VersionSet>> Open(Env* env,
                                                     std::string db_path);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  Status LogAndApply(const VersionEdit& edit);
  [[nodiscard]] std::uint64_t NewFileNumber();
  [[nodiscard]] std::uint64_t last_sequence() const { return last_sequence_; }
  [[nodiscard]] std::uint64_t next_file_number() const {
    return next_file_number_;
  }
  [[nodiscard]] std::vector<FileMeta> Files() const;
  [[nodiscard]] const std::string& manifest_name() const {
    return manifest_name_;
  }
  [[nodiscard]] const std::string& db_path() const { return db_path_; }

  static Status EncodeEdit(const VersionEdit& edit,
                           std::uint64_t last_sequence,
                           std::uint64_t next_file_number,
                           std::string* encoded);
  static Status DecodeEdit(std::string_view encoded, VersionEdit* edit,
                           std::uint64_t* last_sequence,
                           std::uint64_t* next_file_number);

 private:
  VersionSet(Env* env, std::string db_path);
  Status Initialize();
  Status ApplyEdit(const VersionEdit& edit,
                   std::vector<std::vector<FileMeta>>* files) const;
  Status SetCurrentFile();

  Env* env_;
  std::string db_path_;
  std::string manifest_name_;
  std::unique_ptr<WAL> manifest_;
  std::vector<std::vector<FileMeta>> files_;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t next_file_number_ = 1;
};

std::string TableFileName(const std::string& db_path,
                          std::uint64_t file_number);

}  // namespace microfs
