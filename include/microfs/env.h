#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "microfs/status.h"

namespace microfs {

class Env {
 public:
  virtual ~Env() = default;

  virtual Status ReadFile(const std::string& path,
                          std::vector<std::byte>* contents) = 0;
  virtual Status WriteFile(const std::string& path,
                           std::span<const std::byte> contents) = 0;
  virtual Status AppendFile(const std::string& path,
                            std::span<const std::byte> contents) = 0;
  virtual Status SyncFile(const std::string& path) = 0;
  virtual Status TruncateFile(const std::string& path, std::uint64_t size) = 0;
  virtual Status RenameFile(const std::string& from,
                            const std::string& to) = 0;
  virtual Status RemoveFile(const std::string& path) = 0;
  virtual Status FileExists(const std::string& path, bool* exists) = 0;
  virtual Status CreateDirectories(const std::string& path) = 0;
};

std::unique_ptr<Env> NewDefaultEnv();

class FaultInjectingEnv final : public Env {
 public:
  explicit FaultInjectingEnv(std::unique_ptr<Env> base);

  Status ReadFile(const std::string& path,
                  std::vector<std::byte>* contents) override;
  Status WriteFile(const std::string& path,
                   std::span<const std::byte> contents) override;
  Status AppendFile(const std::string& path,
                    std::span<const std::byte> contents) override;
  Status SyncFile(const std::string& path) override;
  Status TruncateFile(const std::string& path, std::uint64_t size) override;
  Status RenameFile(const std::string& from, const std::string& to) override;
  Status RemoveFile(const std::string& path) override;
  Status FileExists(const std::string& path, bool* exists) override;
  Status CreateDirectories(const std::string& path) override;

  void Crash();
  void TearNextWrite(std::size_t bytes);
  void FailAfter(std::size_t successful_operations);
  void DisableFailures();

 private:
  Status MaybeFail();
  Status FlushPending(const std::string& path);

  std::unique_ptr<Env> base_;
  std::unordered_map<std::string, std::vector<std::byte>> pending_;
  std::size_t operations_before_failure_ = static_cast<std::size_t>(-1);
  std::size_t tear_next_write_ = static_cast<std::size_t>(-1);
};

}  // namespace microfs
