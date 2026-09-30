#include "workspace.hpp"

#include "proxy.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

namespace khonsu::native {

namespace {

// Never captured, on top of .gitignore: VCS metadata, Khonsu's own files, bulky
// caches, and files that conventionally hold secrets (a recording is meant to be
// shared; credentials must not ride along).
constexpr const char* kDefaultIgnore[] = {
    ".git", ".hg", ".svn", ".khonsu", "*.khn", "node_modules", "__pycache__", ".venv", "venv", ".tox",
    ".mypy_cache", ".pytest_cache", ".ruff_cache", ".DS_Store", ".idea", "*.pyc", "*.swp",
    // secrets
    ".env", ".env.*", "*.pem", "*.key", "*.p12", "*.pfx", "id_rsa*", "id_ed25519*", "id_ecdsa*", ".netrc",
    ".pypirc", ".npmrc", "credentials", "credentials.json", "*.keystore", "secrets.*"};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t mtime_ns(const struct stat& st) {
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

bool read_fd(int fd, std::uint64_t size, std::vector<std::byte>& out) {
  out.resize(static_cast<std::size_t>(size));
  std::size_t got = 0;
  while (got < out.size()) {
    const ssize_t r = ::read(fd, out.data() + got, out.size() - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += static_cast<std::size_t>(r);
  }
  out.resize(got);  // file shrank while reading: take what exists (next snapshot will see it)
  return true;
}

bool write_all_fd(int fd, const std::byte* p, std::size_t n) {
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

bool mkdirs(const std::string& path) {
  if (path.empty()) return true;
  struct stat st {};
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !mkdirs(path.substr(0, slash))) return false;
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

}  // namespace

// =========================================================================================
// Snapshotter
// =========================================================================================

namespace {
bool parse_rule_line(std::string line, bool& negate, bool& dir_only, bool& anchored, std::string& pattern) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  // Trailing spaces are ignored unless escaped.
  while (!line.empty() && line.back() == ' ' && (line.size() < 2 || line[line.size() - 2] != '\\')) line.pop_back();
  if (line.empty() || line[0] == '#') return false;
  negate = line[0] == '!';
  if (negate || line.rfind("\\!", 0) == 0 || line.rfind("\\#", 0) == 0) line.erase(0, 1);
  dir_only = !line.empty() && line.back() == '/';
  while (!line.empty() && line.back() == '/') line.pop_back();
  anchored = line.find('/') != std::string::npos;  // a slash anywhere but the end anchors to `base`
  while (!line.empty() && line.front() == '/') line.erase(0, 1);
  if (line.rfind("**/", 0) == 0 && line.find('/', 3) == std::string::npos) {
    line.erase(0, 3);  // "**/name" == "name" at any depth
    anchored = false;
  }
  pattern = line;
  return !pattern.empty();
}
}  // namespace

void Snapshotter::load_rules(const std::string& file, const std::string& base) {
  FILE* f = std::fopen(file.c_str(), "r");
  if (!f) return;
  char buf[4096];
  while (std::fgets(buf, sizeof buf, f)) {
    Rule r;
    r.base = base;
    if (parse_rule_line(buf, r.negate, r.dir_only, r.anchored, r.pattern)) rules_.push_back(std::move(r));
  }
  std::fclose(f);
}

Snapshotter::Snapshotter(std::string root, SnapshotOptions opts) : root_(std::move(root)), opts_(std::move(opts)) {
  auto add = [&](const std::string& p) {
    Rule r;
    if (parse_rule_line(p, r.negate, r.dir_only, r.anchored, r.pattern)) rules_.push_back(std::move(r));
  };
  for (const char* p : kDefaultIgnore) add(p);
  for (const auto& p : opts_.ignore) add(p);
  load_rules(root_ + "/.git/info/exclude", "");
  load_rules(root_ + "/.khonsuignore", "");
}

bool Snapshotter::ignored(const std::string& name, const std::string& rel, bool is_dir) const {
  bool ignore = false;
  for (const auto& r : rules_) {
    if (r.dir_only && !is_dir) continue;
    // Rules from a nested ignore file only see paths under their directory.
    std::string_view sub = rel;
    if (!r.base.empty()) {
      if (rel.size() <= r.base.size() || rel.compare(0, r.base.size(), r.base) != 0 || rel[r.base.size()] != '/')
        continue;
      sub = std::string_view(rel).substr(r.base.size() + 1);
    }
    bool hit;
    if (r.anchored) {
      const bool globstar = r.pattern.find("**") != std::string::npos;  // '**' may span directories
      hit = ::fnmatch(r.pattern.c_str(), std::string(sub).c_str(), globstar ? 0 : FNM_PATHNAME) == 0;
    } else {
      hit = ::fnmatch(r.pattern.c_str(), name.c_str(), 0) == 0;
    }
    if (hit) ignore = !r.negate;  // last match wins
  }
  return ignore;
}

Digest128 Snapshotter::walk(int dirfd, const std::string& rel, ws::Snapshot& s, const ObjectSink& sink,
                            std::int64_t racy_after_ns) {
  std::vector<ws::Entry> entries;
  DIR* d = ::fdopendir(dirfd);  // takes ownership of dirfd
  if (!d) {
    ::close(dirfd);
    return {};
  }
  std::vector<std::string> names;
  while (dirent* de = ::readdir(d)) {
    const std::string name = de->d_name;
    if (name == "." || name == "..") continue;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  // A nested .gitignore applies to this directory and everything below it.
  const std::size_t rules_before = rules_.size();
  if (std::binary_search(names.begin(), names.end(), std::string(".gitignore")))
    load_rules(root_ + (rel.empty() ? "" : "/" + rel) + "/.gitignore", rel);

  auto emit = [&](ws::ObjectKind kind, const Digest128& id, std::vector<std::byte>&& bytes) {
    if (emitted_.contains(id)) return;
    emitted_[id] = true;
    s.new_objects += 1;
    s.new_bytes += bytes.size();
    sink(kind, id, std::move(bytes));
  };

  for (const auto& name : names) {
    const std::string path = rel.empty() ? name : rel + "/" + name;
    struct stat st {};
    if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;  // vanished mid-walk
    if (ignored(name, path, S_ISDIR(st.st_mode))) continue;
    ws::Entry e;
    e.name = name;
    if (S_ISDIR(st.st_mode)) {
      const int sub = ::openat(::dirfd(d), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (sub < 0) continue;
      e.type = ws::EntryType::Dir;
      e.mode = 0755;
      e.id = walk(sub, path, s, sink, racy_after_ns);
    } else if (S_ISLNK(st.st_mode)) {
      char target[4096];
      const ssize_t n = ::readlinkat(::dirfd(d), name.c_str(), target, sizeof target);
      if (n < 0) continue;
      std::vector<std::byte> bytes(reinterpret_cast<std::byte*>(target), reinterpret_cast<std::byte*>(target) + n);
      e.type = ws::EntryType::Symlink;
      e.mode = 0777;
      e.size = static_cast<std::uint64_t>(n);
      e.id = ws::blob_id(bytes);
      emit(ws::ObjectKind::Blob, e.id, std::move(bytes));
    } else if (S_ISREG(st.st_mode)) {
      e.mode = (st.st_mode & 0111) ? 0755 : 0644;  // only the executable bit is meaningful
      e.size = static_cast<std::uint64_t>(st.st_size);
      s.files += 1;
      s.bytes += e.size;
      const std::int64_t mt = mtime_ns(st);
      auto& c = cache_[path];
      const bool fresh = c.size == e.size && c.mtime_ns == static_cast<std::uint64_t>(mt) &&
                         c.ino == static_cast<std::uint64_t>(st.st_ino) &&
                         c.dev == static_cast<std::uint64_t>(st.st_dev) && mt < racy_after_ns && !c.id.empty();
      const bool large = e.size > opts_.max_file_bytes;
      if (large) e.type = ws::EntryType::Omitted;  // tracked by content hash, contents not stored
      if (fresh && (large || emitted_.contains(c.id))) {
        e.id = c.id;
      } else {
        const int fd = ::openat(::dirfd(d), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        if (large) {
          // Stream-hash so copies of the same large file compare equal across directories.
          DigestBuilder h(domain::kBlob);
          std::vector<std::byte> buf(1u << 20);
          std::uint64_t total = 0;
          for (;;) {
            const ssize_t r = ::read(fd, buf.data(), buf.size());
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            h.bytes({buf.data(), static_cast<std::size_t>(r)});
            total += static_cast<std::uint64_t>(r);
          }
          h.u64(total);
          e.id = h.finish();
        } else {
          std::vector<std::byte> bytes;
          read_fd(fd, e.size, bytes);
          e.size = bytes.size();
          e.id = ws::blob_id(bytes);
          emit(ws::ObjectKind::Blob, e.id, std::move(bytes));
        }
        ::close(fd);
        c = {e.size, static_cast<std::uint64_t>(mt), static_cast<std::uint64_t>(st.st_ino),
             static_cast<std::uint64_t>(st.st_dev), e.id};
      }
    } else {
      continue;  // sockets, fifos, devices: not workspace content
    }
    entries.push_back(std::move(e));
  }
  ::closedir(d);
  rules_.resize(rules_before);  // leaving the directory: its .gitignore no longer applies

  auto tree = ws::encode_tree(std::move(entries));
  const Digest128 id = ws::tree_id(tree);
  trees_.put(ws::ObjectKind::Tree, id, tree);
  emit(ws::ObjectKind::Tree, id, std::move(tree));
  return id;
}

ws::Snapshot Snapshotter::take(ws::When when, std::uint64_t seq, const ObjectSink& sink) {
  const auto t0 = std::chrono::steady_clock::now();
  ws::Snapshot s;
  s.when = when;
  s.seq = seq;
  const int fd = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) s.root = walk(fd, "", s, sink, now_ns() - 2'000'000'000);
  s.took_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  return s;
}

// =========================================================================================
// Checkout
// =========================================================================================

namespace {
bool checkout_tree(const ws::ObjectMap& m, const Digest128& id, const std::string& dir, std::vector<std::string>* skipped,
                   const std::string& rel, std::string* error) {
  const auto* bytes = m.get(id);
  if (!bytes) {
    if (error) *error = "tree object missing for " + (rel.empty() ? std::string(".") : rel);
    return false;
  }
  const auto entries = ws::decode_tree(*bytes);
  if (!entries) {
    if (error) *error = "corrupt tree object";
    return false;
  }
  if (!mkdirs(dir)) {
    if (error) *error = dir + ": " + std::strerror(errno);
    return false;
  }
  std::vector<std::string_view> seen;
  for (const auto& e : *entries) {
    const std::string path = dir + "/" + e.name;
    const std::string r = rel.empty() ? e.name : rel + "/" + e.name;
    // No path traversal via crafted recordings: no separators, no dot names, no duplicates
    // (a symlink "x" followed by a directory "x" would otherwise write through the link).
    if (e.name.empty() || e.name.find('/') != std::string::npos || e.name.find('\0') != std::string::npos ||
        e.name == ".." || e.name == "." || std::find(seen.begin(), seen.end(), e.name) != seen.end()) {
      if (error) *error = "refusing unsafe entry name: " + e.name;
      return false;
    }
    seen.push_back(e.name);
    switch (e.type) {
      case ws::EntryType::Dir: {
        struct stat st {};
        if (::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
          if (error) *error = "refusing to write through the symlink " + path;
          return false;
        }
        if (!checkout_tree(m, e.id, path, skipped, r, error)) return false;
        break;
      }
      case ws::EntryType::Omitted:
        if (skipped) skipped->push_back(r);
        break;
      case ws::EntryType::Symlink: {
        const auto* target = m.get(e.id);
        if (!target) {
          if (skipped) skipped->push_back(r);
          break;
        }
        ::unlink(path.c_str());
        const std::string t(reinterpret_cast<const char*>(target->data()), target->size());
        if (::symlink(t.c_str(), path.c_str()) != 0 && skipped) skipped->push_back(r);
        break;
      }
      case ws::EntryType::File: {
        const auto* content = m.get(e.id);
        if (!content) {
          if (error) *error = "blob missing for " + r;
          return false;
        }
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, e.mode);
        if (fd < 0 || !write_all_fd(fd, content->data(), content->size())) {
          if (fd >= 0) ::close(fd);
          if (error) *error = path + ": " + std::strerror(errno);
          return false;
        }
        ::fchmod(fd, e.mode);
        ::close(fd);
        break;
      }
    }
  }
  return true;
}
}  // namespace

bool checkout(const ws::ObjectMap& objects, const Digest128& root, const std::string& dir, bool force,
              std::vector<std::string>* skipped, std::string* error) {
  if (!force) {
    if (DIR* d = ::opendir(dir.c_str())) {
      bool empty = true;
      while (dirent* de = ::readdir(d))
        if (std::strcmp(de->d_name, ".") != 0 && std::strcmp(de->d_name, "..") != 0) empty = false;
      ::closedir(d);
      if (!empty) {
        if (error) *error = dir + " is not empty (use --force to overwrite)";
        return false;
      }
    }
  }
  return checkout_tree(objects, root, dir, skipped, "", error);
}

std::string describe_changes(const std::vector<ws::FileChange>& changes, std::size_t max) {
  std::string out;
  for (std::size_t i = 0; i < changes.size() && i < max; ++i) {
    if (i) out += ", ";
    out += std::string(1, changes[i].kind == 'A' ? '+' : changes[i].kind == 'D' ? '-' : '~') + changes[i].path;
  }
  if (changes.size() > max) out += ", … " + std::to_string(changes.size() - max) + " more";
  return out;
}

// =========================================================================================
// WorkspaceTracker
// =========================================================================================

WorkspaceTracker::WorkspaceTracker(Snapshotter& snap, RecordingWriter* writer, const rec::Recording* recorded,
                                   bool quiet)
    : snap_(snap), writer_(writer), recorded_(recorded), quiet_(quiet) {}

void WorkspaceTracker::take(ws::When when, std::uint64_t seq, const ws::Snapshot* expected, const std::string& label) {
  std::lock_guard lock(mu_);
  const ws::Snapshot s = snap_.take(when, seq, [&](ws::ObjectKind kind, const Digest128& id, std::vector<std::byte>&& b) {
    if (writer_) writer_->object(kind, id, b);
  });
  if (writer_) writer_->snapshot(s);
  ++snapshots_;
  if (!expected || expected->root == s.root) return;
  ++divergences_;
  // Recorded trees come from the recording; live trees from the snapshotter.
  const auto changes = ws::diff(recorded_->objects, expected->root, snap_.trees(), s.root);
  if (!quiet_)
    std::fprintf(stderr, "\033[2m[khonsu]\033[0m \033[33mworkspace differs from the recording %s: %s\033[0m\n",
                 label.c_str(), describe_changes(changes).c_str());
}

void WorkspaceTracker::before_request(std::uint64_t live_seq, const std::uint64_t* recorded_seq) {
  const ws::Snapshot* expected =
      recorded_ && recorded_seq ? recorded_->snapshot_before(*recorded_seq) : nullptr;
  take(ws::When::BeforeRequest, live_seq, expected,
       "before request #" + std::to_string(recorded_seq ? *recorded_seq : live_seq));
}

void WorkspaceTracker::at_start() {
  take(ws::When::RunStart, 0, recorded_ ? recorded_->snapshot_at(ws::When::RunStart) : nullptr, "at run start");
}

void WorkspaceTracker::at_end(bool compare) {
  take(ws::When::RunEnd, 0, recorded_ && compare ? recorded_->snapshot_at(ws::When::RunEnd) : nullptr, "at run end");
}

}  // namespace khonsu::native
