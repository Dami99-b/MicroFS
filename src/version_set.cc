#include "microfs/version_set.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <utility>

#include "microfs/coding.h"
#include "microfs/skiplist.h"

namespace microfs {
namespace {

constexpr std::uint32_t kVersionEditFormat = 1;

bool ReadFixed32(std::string_view* input, std::uint32_t* value) {
  if (input->size() < 4) return false;
  *value = DecodeFixed32(input->data());
  input->remove_prefix(4);
  return true;
}

bool ReadFixed64(std::string_view* input, std::uint64_t* value) {
  if (input->size() < 8) return false;
  *value = DecodeFixed64(input->data());
  input->remove_prefix(8);
  return true;
}

std::string JoinPath(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

bool IsManifestName(std::string_view name) {
  return name.starts_with("MANIFEST-") && name.size() > 9 &&
         name.find_first_of("/\\") == std::string_view::npos &&
         std::all_of(name.begin() + 9, name.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; });
}

}  // namespace

VersionSet::VersionSet(Env* env, std::string db_path)
    : env_(env), db_path_(std::move(db_path)), files_(kNumLevels) {}

StatusOr<std::unique_ptr<VersionSet>> VersionSet::Open(
    Env* env, std::string db_path) {
  if (env == nullptr || db_path.empty()) {
    return Status::InvalidArgument("VersionSet requires an Env and a path");
  }
  try {
    auto versions =
        std::unique_ptr<VersionSet>(new VersionSet(env, std::move(db_path)));
    Status status = versions->Initialize();
    if (!status.ok()) return status;
    return versions;
  } catch (...) {
    return Status::ResourceExhausted("opening VersionSet failed");
  }
}

Status VersionSet::Initialize() {
  Status status = env_->CreateDirectories(db_path_);
  if (!status.ok()) return status;
  const std::string current_path = JoinPath(db_path_, "CURRENT");
  std::vector<std::byte> current_data;
  status = env_->ReadFile(current_path, &current_data);
  const bool has_current = status.ok();
  if (!has_current && status.code() != StatusCode::kNotFound) return status;
  if (has_current) {
    if (current_data.empty()) return Status::Corruption("CURRENT file is empty");
    const std::string current(reinterpret_cast<const char*>(current_data.data()),
                              current_data.size());
    const std::size_t newline = current.find('\n');
    const std::string_view manifest =
        newline == std::string::npos
            ? std::string_view(current)
            : std::string_view(current).substr(0, newline);
    if (!IsManifestName(manifest) ||
        (newline != std::string::npos &&
         current.find_first_not_of("\r\n", newline) != std::string::npos)) {
      return Status::Corruption("CURRENT contains an invalid manifest name");
    }
    manifest_name_.assign(manifest);
  } else {
    manifest_name_ = "MANIFEST-000001";
  }

  const std::string manifest_path = JoinPath(db_path_, manifest_name_);
  if (has_current) {
    bool exists = false;
    status = env_->FileExists(manifest_path, &exists);
    if (!status.ok()) return status;
    if (!exists) return Status::Corruption("CURRENT manifest is missing");
  }
  std::vector<std::string> records;
  status = WAL::Recover(env_, manifest_path, &records);
  if (status.code() == StatusCode::kNotFound && !has_current) {
    status = Status::OK();
  } else if (!status.ok()) {
    return status;
  }
  for (const std::string& record : records) {
    VersionEdit edit;
    std::uint64_t last_sequence = 0;
    std::uint64_t next_file_number = 0;
    status = DecodeEdit(record, &edit, &last_sequence, &next_file_number);
    if (!status.ok()) return status;
    for (const FileMeta& file : edit.added_files) {
      if (file.number >= next_file_number) {
        return Status::Corruption("manifest next file number is inconsistent");
      }
    }
    status = ApplyEdit(edit, &files_);
    if (!status.ok()) return status;
    last_sequence_ = last_sequence;
    next_file_number_ = next_file_number;
  }
  if (has_current && records.empty()) {
    return Status::Corruption("CURRENT manifest contains no complete version edit");
  }

  auto manifest = WAL::Open(env_, manifest_path);
  if (!manifest.ok()) return manifest.status();
  manifest_ = std::move(manifest).value();
  if (records.empty()) {
    VersionEdit initial;
    initial.last_sequence = 0;
    initial.next_file_number = 2;
    status = LogAndApply(initial);
    if (!status.ok()) return status;
    next_file_number_ = 2;
  }
  if (!has_current) return SetCurrentFile();
  return Status::OK();
}

Status VersionSet::SetCurrentFile() {
  const std::string temporary_path = JoinPath(db_path_, "CURRENT.tmp");
  const std::string current_path = JoinPath(db_path_, "CURRENT");
  std::string contents;
  try {
    contents = manifest_name_ + "\n";
  } catch (...) {
    return Status::ResourceExhausted("building CURRENT file failed");
  }
  const auto* bytes = reinterpret_cast<const std::byte*>(contents.data());
  Status status = env_->WriteFile(temporary_path, {bytes, contents.size()});
  if (!status.ok()) return status;
  status = env_->SyncFile(temporary_path);
  if (!status.ok()) return status;
  status = env_->RenameFile(temporary_path, current_path);
  if (!status.ok()) return status;
  return Status::OK();
}

Status VersionSet::ApplyEdit(const VersionEdit& edit,
                             std::vector<std::vector<FileMeta>>* files) const {
  if (files == nullptr || files->size() != kNumLevels) {
    return Status::InvalidArgument("invalid version file set");
  }
  try {
    for (const auto& [level, number] : edit.deleted_files) {
      if (level >= kNumLevels) {
        return Status::Corruption("manifest deletion has invalid level");
      }
      auto& level_files = (*files)[level];
      const auto found = std::find_if(
          level_files.begin(), level_files.end(),
          [number](const FileMeta& file) { return file.number == number; });
      if (found == level_files.end()) {
        return Status::Corruption("manifest deletes an unknown table");
      }
      level_files.erase(found);
    }
    for (const FileMeta& file : edit.added_files) {
      if (file.level >= kNumLevels || file.number == 0 ||
          file.file_size == 0 ||
          LexicographicComparator()(file.smallest_key, file.largest_key) > 0) {
        return Status::Corruption("manifest adds invalid table metadata");
      }
      for (const auto& level_files : *files) {
        if (std::any_of(level_files.begin(), level_files.end(),
                        [&file](const FileMeta& existing) {
                          return existing.number == file.number;
                        })) {
          return Status::Corruption("manifest adds a duplicate table number");
        }
      }
      auto& level_files = (*files)[file.level];
      if (file.level != 0 &&
          std::any_of(level_files.begin(), level_files.end(),
                      [&file](const FileMeta& existing) {
                        return LexicographicComparator()(
                                   existing.smallest_key,
                                   file.largest_key) <= 0 &&
                               LexicographicComparator()(
                                   file.smallest_key,
                                   existing.largest_key) <= 0;
                      })) {
        return Status::Corruption("manifest adds overlapping level files");
      }
      level_files.push_back(file);
      if (file.level == 0) {
        std::sort(level_files.begin(), level_files.end(),
                  [](const FileMeta& lhs, const FileMeta& rhs) {
                    return lhs.number < rhs.number;
                  });
      } else {
        std::sort(level_files.begin(), level_files.end(),
                  [](const FileMeta& lhs, const FileMeta& rhs) {
                    return LexicographicComparator()(lhs.smallest_key,
                                                     rhs.smallest_key) < 0;
                  });
      }
    }
  } catch (...) {
    return Status::ResourceExhausted("applying manifest edit failed");
  }
  return Status::OK();
}

Status VersionSet::LogAndApply(const VersionEdit& edit) {
  const std::uint64_t next_last_sequence =
      edit.last_sequence.value_or(last_sequence_);
  const std::uint64_t next_file_number =
      edit.next_file_number.value_or(next_file_number_);
  if (next_file_number == 0) {
    return Status::InvalidArgument("next file number must be nonzero");
  }
  for (const FileMeta& file : edit.added_files) {
    if (file.number >= next_file_number) {
      return Status::InvalidArgument(
          "next file number must exceed every added table number");
    }
  }
  std::vector<std::vector<FileMeta>> updated_files;
  std::string encoded;
  try {
    updated_files = files_;
  } catch (...) {
    return Status::ResourceExhausted("copying version state failed");
  }
  Status status = ApplyEdit(edit, &updated_files);
  if (!status.ok()) return status;
  status = EncodeEdit(edit, next_last_sequence, next_file_number, &encoded);
  if (!status.ok()) return status;
  status = manifest_->Append(encoded);
  if (!status.ok()) return status;
  files_.swap(updated_files);
  last_sequence_ = next_last_sequence;
  next_file_number_ = next_file_number;
  return Status::OK();
}

std::uint64_t VersionSet::NewFileNumber() {
  if (next_file_number_ == std::numeric_limits<std::uint64_t>::max()) return 0;
  return next_file_number_++;
}

std::vector<FileMeta> VersionSet::Files() const {
  std::vector<FileMeta> files;
  for (const auto& level : files_) {
    files.insert(files.end(), level.begin(), level.end());
  }
  return files;
}

Status VersionSet::EncodeEdit(const VersionEdit& edit,
                              std::uint64_t last_sequence,
                              std::uint64_t next_file_number,
                              std::string* encoded) {
  if (encoded == nullptr || next_file_number == 0) {
    return Status::InvalidArgument("invalid manifest encoding arguments");
  }
  try {
    std::string result;
    PutFixed32(&result, kVersionEditFormat);
    PutFixed64(&result, last_sequence);
    PutFixed64(&result, next_file_number);
    PutVarint64(&result, edit.added_files.size());
    for (const FileMeta& file : edit.added_files) {
      PutFixed32(&result, file.level);
      PutFixed64(&result, file.number);
      PutFixed64(&result, file.file_size);
      PutLengthPrefixed(&result, file.smallest_key);
      PutLengthPrefixed(&result, file.largest_key);
    }
    PutVarint64(&result, edit.deleted_files.size());
    for (const auto& [level, number] : edit.deleted_files) {
      PutFixed32(&result, level);
      PutFixed64(&result, number);
    }
    *encoded = std::move(result);
  } catch (...) {
    return Status::ResourceExhausted("encoding manifest edit failed");
  }
  return Status::OK();
}

Status VersionSet::DecodeEdit(std::string_view encoded, VersionEdit* edit,
                              std::uint64_t* last_sequence,
                              std::uint64_t* next_file_number) {
  if (edit == nullptr || last_sequence == nullptr ||
      next_file_number == nullptr) {
    return Status::InvalidArgument("null manifest decode output");
  }
  std::uint32_t version = 0;
  if (!ReadFixed32(&encoded, &version) || version != kVersionEditFormat ||
      !ReadFixed64(&encoded, last_sequence) ||
      !ReadFixed64(&encoded, next_file_number) || *next_file_number == 0) {
    return Status::Corruption("invalid manifest edit header");
  }
  std::uint64_t count = 0;
  if (!GetVarint64(&encoded, &count) || count > encoded.size() / 20) {
    return Status::Corruption("invalid manifest added-file count");
  }
  VersionEdit result;
  result.last_sequence = *last_sequence;
  result.next_file_number = *next_file_number;
  try {
    result.added_files.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      FileMeta file;
      std::uint32_t level = 0;
      std::string_view smallest;
      std::string_view largest;
      if (!ReadFixed32(&encoded, &level) ||
          !ReadFixed64(&encoded, &file.number) ||
          !ReadFixed64(&encoded, &file.file_size) ||
          !GetLengthPrefixed(&encoded, &smallest) ||
          !GetLengthPrefixed(&encoded, &largest)) {
        return Status::Corruption("truncated manifest file addition");
      }
      file.level = level;
      file.smallest_key.assign(smallest);
      file.largest_key.assign(largest);
      result.added_files.push_back(std::move(file));
    }
    if (!GetVarint64(&encoded, &count) || count > encoded.size() / 12) {
      return Status::Corruption("invalid manifest deleted-file count");
    }
    result.deleted_files.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint32_t level = 0;
      std::uint64_t number = 0;
      if (!ReadFixed32(&encoded, &level) ||
          !ReadFixed64(&encoded, &number)) {
        return Status::Corruption("truncated manifest file deletion");
      }
      result.deleted_files.emplace_back(level, number);
    }
  } catch (...) {
    return Status::ResourceExhausted("decoding manifest edit failed");
  }
  if (!encoded.empty()) return Status::Corruption("manifest edit has trailing bytes");
  *edit = std::move(result);
  return Status::OK();
}

std::string TableFileName(const std::string& db_path,
                          std::uint64_t file_number) {
  std::string name = "TABLE-";
  const std::string digits = std::to_string(file_number);
  if (digits.size() < 6) name.append(6 - digits.size(), '0');
  name.append(digits);
  name.append(".sst");
  return JoinPath(db_path, name);
}

}  // namespace microfs
