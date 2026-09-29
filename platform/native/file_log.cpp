#include "file_log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace khonsu::native {

namespace {

bool write_all(int fd, const std::byte* p, std::size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

int data_sync(int fd) {
#if defined(__APPLE__)
  return ::fcntl(fd, F_FULLFSYNC);  // fsync() on macOS doesn't flush the drive cache
#else
  return ::fdatasync(fd);
#endif
}

// fsync the directory so a newly created file's directory entry is durable too.
void sync_parent_dir(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
  const int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (d >= 0) {
    ::fsync(d);
    ::close(d);
  }
}

}  // namespace

bool read_file(const std::string& path, std::vector<std::byte>& out, std::string* error) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (error) *error = path + ": " + std::strerror(errno);
    return false;
  }
  struct stat st {};
  ::fstat(fd, &st);
  out.resize(static_cast<std::size_t>(st.st_size));
  std::size_t got = 0;
  while (got < out.size()) {
    const ssize_t r = ::read(fd, out.data() + got, out.size() - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += static_cast<std::size_t>(r);
  }
  out.resize(got);
  ::close(fd);
  return true;
}

FileLog::~FileLog() { close(); }

bool FileLog::open(const std::string& path, SyncPolicy policy, std::string* error, RecoveryReport* report) {
  std::lock_guard lock(mu_);
  path_ = path;
  policy_ = policy;
  const bool existed = ::access(path.c_str(), F_OK) == 0;
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    if (error) *error = path + ": " + std::strerror(errno);
    return false;
  }
  if (!existed || ::lseek(fd_, 0, SEEK_END) == 0) {
    const auto hdr = EventLog::file_header();
    if (!write_all(fd_, hdr.data(), hdr.size()) || data_sync(fd_) != 0) {
      if (error) *error = path + ": cannot write header: " + std::strerror(errno);
      return false;
    }
    sync_parent_dir(path);
    bytes_ = hdr.size();
    return true;
  }

  // Recovery: keep the longest valid prefix, truncate the rest.
  std::vector<std::byte> data;
  if (!read_file(path, data, error)) return false;
  RecoveryReport rep;
  const EventLog log = EventLog::parse(data, &rep);
  if (log.size() == 0 && data.size() >= 16 && rep.reason == "bad file header") {
    if (error) *error = path + ": not a Khonsu log";
    return false;
  }
  const std::uint64_t keep = data.size() - rep.bytes_discarded;
  if (keep < 16) {  // crashed while writing the file header itself: start over
    const auto hdr = EventLog::file_header();
    if (::ftruncate(fd_, 0) != 0 || ::lseek(fd_, 0, SEEK_SET) != 0 || !write_all(fd_, hdr.data(), hdr.size()) ||
        data_sync(fd_) != 0) {
      if (error) *error = path + ": cannot rewrite header: " + std::strerror(errno);
      return false;
    }
    bytes_ = hdr.size();
    if (report) *report = rep;
    return true;
  }
  if (rep.bytes_discarded > 0 && (::ftruncate(fd_, static_cast<off_t>(keep)) != 0 || data_sync(fd_) != 0)) {
    if (error) *error = path + ": cannot truncate torn tail: " + std::strerror(errno);
    return false;
  }
  ::lseek(fd_, static_cast<off_t>(keep), SEEK_SET);
  lsn_ = log.size();
  bytes_ = keep;
  last_crc_ = lsn_ ? log.frame(lsn_ - 1).header.crc32c : 0;
  if (report) *report = rep;
  return true;
}

void FileLog::close() {
  std::lock_guard lock(mu_);
  if (fd_ >= 0) {
    if (policy_ != SyncPolicy::None) data_sync(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

std::uint64_t FileLog::append(FrameType type, std::uint16_t actor, std::uint64_t t_us,
                              std::span<const std::byte> payload) {
  std::lock_guard lock(mu_);
  const std::uint64_t lsn = lsn_;
  const auto frame = EventLog::encode_frame(lsn, type, actor, t_us, payload);
  if (fd_ < 0 || !write_all(fd_, frame.data(), frame.size())) {
    failed_ = true;
    return lsn;
  }
  std::memcpy(&last_crc_, frame.data() + 4, 4);
  ++lsn_;
  bytes_ += frame.size();
  return lsn;
}

bool FileLog::sync() {
  std::lock_guard lock(mu_);
  if (fd_ < 0 || failed_) return false;
  if (policy_ == SyncPolicy::None) return true;
  return data_sync(fd_) == 0;
}

std::uint32_t FileLog::last_crc() const {
  std::lock_guard lock(mu_);
  return last_crc_;
}
std::uint64_t FileLog::frames() const {
  std::lock_guard lock(mu_);
  return lsn_;
}
std::uint64_t FileLog::bytes() const {
  std::lock_guard lock(mu_);
  return bytes_;
}

}  // namespace khonsu::native
