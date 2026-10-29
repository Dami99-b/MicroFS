#include "microfs/env.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace microfs {
namespace {

std::string ErrnoMessage(const std::string& operation,
                         const std::string& path) {
  return operation + " '" + path + "': " + std::strerror(errno);
}

class StandardEnv final : public Env {
 public:
  Status ReadFile(const std::string& path,
                  std::vector<std::byte>* contents) override {
    if (contents == nullptr) return Status::InvalidArgument("null output");
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      if (errno == ENOENT) return Status::NotFound(ErrnoMessage("open", path));
      return Status::IOError(ErrnoMessage("open", path));
    }
    std::vector<std::byte> result;
    std::byte buffer[8192];
    while (true) {
      const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file);
      try {
        result.insert(result.end(), buffer, buffer + count);
      } catch (...) {
        std::fclose(file);
        return Status::ResourceExhausted("reading file exhausted memory");
      }
      if (count < sizeof(buffer)) {
        if (std::ferror(file) != 0) {
          const std::string message = ErrnoMessage("read", path);
          std::fclose(file);
          return Status::IOError(message);
        }
        break;
      }
    }
    if (std::fclose(file) != 0) {
      return Status::IOError(ErrnoMessage("close", path));
    }
    *contents = std::move(result);
    return Status::OK();
  }

  Status WriteFile(const std::string& path,
                   std::span<const std::byte> contents) override {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return Status::IOError(ErrnoMessage("open", path));
    const bool failed = !contents.empty() &&
                        std::fwrite(contents.data(), 1, contents.size(), file) !=
                            contents.size();
    if (std::fclose(file) != 0 || failed) {
      return Status::IOError(ErrnoMessage("write", path));
    }
    return Status::OK();
  }

  Status AppendFile(const std::string& path,
                    std::span<const std::byte> contents) override {
    std::FILE* file = std::fopen(path.c_str(), "ab");
    if (file == nullptr) return Status::IOError(ErrnoMessage("open", path));
    const bool failed = !contents.empty() &&
                        std::fwrite(contents.data(), 1, contents.size(), file) !=
                            contents.size();
    if (std::fclose(file) != 0 || failed) {
      return Status::IOError(ErrnoMessage("append", path));
    }
    return Status::OK();
  }

  Status SyncFile(const std::string& path) override {
#ifdef _WIN32
    const int descriptor = _open(path.c_str(), _O_RDWR | _O_BINARY);
    if (descriptor < 0) return Status::IOError(ErrnoMessage("open", path));
    const int result = _commit(descriptor);
    _close(descriptor);
#else
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) return Status::IOError(ErrnoMessage("open", path));
    const int result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) return Status::IOError(ErrnoMessage("sync", path));
    return Status::OK();
  }

  Status TruncateFile(const std::string& path, std::uint64_t size) override {
    if (size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
      return Status::InvalidArgument("truncate size exceeds platform limit");
    }
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    if (error) return Status::IOError("truncate '" + path + "': " + error.message());
    return Status::OK();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) return Status::IOError("rename: " + error.message());
    return Status::OK();
  }

  Status RemoveFile(const std::string& path) override {
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error) return Status::IOError("remove '" + path + "': " + error.message());
    if (!removed) return Status::NotFound("file does not exist: " + path);
    return Status::OK();
  }

  Status FileExists(const std::string& path, bool* exists) override {
    if (exists == nullptr) return Status::InvalidArgument("null output");
    std::error_code error;
    *exists = std::filesystem::exists(path, error);
    if (error) return Status::IOError("stat '" + path + "': " + error.message());
    return Status::OK();
  }

  Status CreateDirectories(const std::string& path) override {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
      return Status::IOError("create directories '" + path + "': " +
                             error.message());
    }
    return Status::OK();
  }
};

}  // namespace

std::unique_ptr<Env> NewDefaultEnv() { return std::make_unique<StandardEnv>(); }

FaultInjectingEnv::FaultInjectingEnv(std::unique_ptr<Env> base)
    : base_(std::move(base)) {}

Status FaultInjectingEnv::MaybeFail() {
  if (operations_before_failure_ == 0) {
    return Status::IOError("injected I/O failure");
  }
  if (operations_before_failure_ != static_cast<std::size_t>(-1)) {
    --operations_before_failure_;
  }
  return Status::OK();
}

Status FaultInjectingEnv::ReadFile(const std::string& path,
                                   std::vector<std::byte>* contents) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  const auto it = pending_.find(path);
  if (it != pending_.end()) {
    if (contents == nullptr) return Status::InvalidArgument("null output");
    *contents = it->second;
    return Status::OK();
  }
  return base_->ReadFile(path, contents);
}

Status FaultInjectingEnv::WriteFile(const std::string& path,
                                    std::span<const std::byte> contents) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  std::size_t size = contents.size();
  if (tear_next_write_ != static_cast<std::size_t>(-1)) {
    size = std::min(size, tear_next_write_);
    tear_next_write_ = static_cast<std::size_t>(-1);
  }
  try {
    pending_[path] = {contents.begin(), contents.begin() + size};
  } catch (...) {
    return Status::ResourceExhausted("staging write exhausted memory");
  }
  return Status::OK();
}

Status FaultInjectingEnv::AppendFile(const std::string& path,
                                     std::span<const std::byte> contents) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  std::vector<std::byte> data;
  auto it = pending_.find(path);
  if (it == pending_.end()) {
    status = base_->ReadFile(path, &data);
    if (!status.ok() && status.code() != StatusCode::kNotFound) return status;
  } else {
    data = std::move(it->second);
  }
  std::size_t size = contents.size();
  if (tear_next_write_ != static_cast<std::size_t>(-1)) {
    size = std::min(size, tear_next_write_);
    tear_next_write_ = static_cast<std::size_t>(-1);
  }
  try {
    data.insert(data.end(), contents.begin(), contents.begin() + size);
    pending_[path] = std::move(data);
  } catch (...) {
    return Status::ResourceExhausted("staging append exhausted memory");
  }
  return Status::OK();
}

Status FaultInjectingEnv::FlushPending(const std::string& path) {
  const auto it = pending_.find(path);
  if (it == pending_.end()) return Status::OK();
  Status status = base_->WriteFile(path, it->second);
  if (!status.ok()) return status;
  status = base_->SyncFile(path);
  if (status.ok()) pending_.erase(it);
  return status;
}

Status FaultInjectingEnv::SyncFile(const std::string& path) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  return FlushPending(path);
}

Status FaultInjectingEnv::TruncateFile(const std::string& path,
                                       std::uint64_t size) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  std::vector<std::byte> data;
  auto it = pending_.find(path);
  if (it == pending_.end()) {
    status = base_->ReadFile(path, &data);
    if (!status.ok()) return status;
  } else {
    data = std::move(it->second);
  }
  if (size > data.size()) {
    return Status::InvalidArgument("cannot extend a file by truncating");
  }
  data.resize(static_cast<std::size_t>(size));
  pending_[path] = std::move(data);
  return Status::OK();
}

Status FaultInjectingEnv::RenameFile(const std::string& from,
                                     const std::string& to) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  status = FlushPending(from);
  if (!status.ok()) return status;
  return base_->RenameFile(from, to);
}

Status FaultInjectingEnv::RemoveFile(const std::string& path) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  pending_.erase(path);
  return base_->RemoveFile(path);
}

Status FaultInjectingEnv::FileExists(const std::string& path, bool* exists) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  if (pending_.contains(path)) {
    if (exists == nullptr) return Status::InvalidArgument("null output");
    *exists = true;
    return Status::OK();
  }
  return base_->FileExists(path, exists);
}

Status FaultInjectingEnv::CreateDirectories(const std::string& path) {
  Status status = MaybeFail();
  if (!status.ok()) return status;
  return base_->CreateDirectories(path);
}

void FaultInjectingEnv::Crash() { pending_.clear(); }

void FaultInjectingEnv::TearNextWrite(std::size_t bytes) {
  tear_next_write_ = bytes;
}

void FaultInjectingEnv::FailAfter(std::size_t successful_operations) {
  operations_before_failure_ = successful_operations;
}

void FaultInjectingEnv::DisableFailures() {
  operations_before_failure_ = static_cast<std::size_t>(-1);
  tear_next_write_ = static_cast<std::size_t>(-1);
}

}  // namespace microfs
