// Khonsu — durable, append-only event log on disk (POSIX).
//
// Frames are written with a single write() each and made durable with
// fdatasync(). A frame is *acknowledged* once sync() has returned after it was
// appended; recovery guarantees every acknowledged frame survives a crash
// (kill -9, power loss) and discards a torn tail (tests cut the file at every
// byte offset).
#pragma once

#include "khonsu/log.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace khonsu::native {

enum class SyncPolicy { Always, None };

class FileLog {
public:
  FileLog() = default;
  ~FileLog();
  FileLog(const FileLog&) = delete;
  FileLog& operator=(const FileLog&) = delete;

  // Creates a new log, or opens an existing one and recovers it: frames after the
  // first torn/corrupt frame are truncated away. Returns false with `error` set.
  bool open(const std::string& path, SyncPolicy policy, std::string* error, RecoveryReport* report = nullptr);
  void close();

  // Thread-safe. Returns the frame's LSN.
  std::uint64_t append(FrameType type, std::uint16_t actor, std::uint64_t t_us, std::span<const std::byte> payload);
  // Thread-safe. fdatasync() unless the policy is None. Returns false on I/O error.
  bool sync();

  // CRC of the most recently appended frame (for hash chains). Thread-safe.
  [[nodiscard]] std::uint32_t last_crc() const;
  [[nodiscard]] std::uint64_t frames() const;
  [[nodiscard]] std::uint64_t bytes() const;
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] bool ok() const { return fd_ >= 0 && !failed_; }

private:
  mutable std::mutex mu_;
  int fd_ = -1;
  std::string path_;
  SyncPolicy policy_ = SyncPolicy::Always;
  std::uint64_t lsn_ = 0, bytes_ = 0;
  std::uint32_t last_crc_ = 0;
  bool failed_ = false;
};

// Reads a whole file into memory (for loading recordings). Returns false on error.
bool read_file(const std::string& path, std::vector<std::byte>& out, std::string* error);

}  // namespace khonsu::native
