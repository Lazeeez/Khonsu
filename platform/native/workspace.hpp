// Khonsu — capture and restore the agent's working directory (POSIX).
#pragma once

#include "khonsu/recording.hpp"
#include "khonsu/snapshot.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace khonsu::native {

class RecordingWriter;

struct SnapshotOptions {
  std::uint64_t max_file_bytes = 8u << 20;  // larger files are tracked (content hash) but not stored
  std::vector<std::string> ignore;          // extra gitignore-style patterns
};

using ObjectSink = std::function<void(ws::ObjectKind, const Digest128&, std::vector<std::byte>&&)>;

// Walks a directory into content-addressed objects. Re-hashing is avoided for
// files whose (size, mtime, inode) are unchanged since the previous snapshot,
// except "racy" files modified within the last 2 s (same rule git uses, since
// mtime granularity can hide a second write).
class Snapshotter {
public:
  Snapshotter(std::string root, SnapshotOptions opts = {});

  // Takes a snapshot; every object not emitted before goes to `sink`.
  // Tree objects are also kept in trees() for diffing.
  ws::Snapshot take(ws::When when, std::uint64_t seq, const ObjectSink& sink);
  [[nodiscard]] const ws::ObjectMap& trees() const { return trees_; }
  [[nodiscard]] const std::string& root() const { return root_; }

private:
  struct Cached {
    std::uint64_t size = 0, mtime_ns = 0, ino = 0, dev = 0;
    Digest128 id;
  };
  Digest128 walk(int dirfd, const std::string& rel, ws::Snapshot& s, const ObjectSink& sink, std::int64_t racy_after_ns);
  // gitignore semantics: nested files, '!' negation, anchored and dir-only patterns;
  // the last matching rule wins.
  struct Rule {
    std::string base;     // directory (relative to root) whose ignore file defined it; "" = root
    std::string pattern;  // without leading '!' / '/' and trailing '/'
    bool negate = false, dir_only = false, anchored = false;
  };
  void load_rules(const std::string& file, const std::string& base);
  [[nodiscard]] bool ignored(const std::string& name, const std::string& rel, bool is_dir) const;

  std::string root_;
  SnapshotOptions opts_;
  std::vector<Rule> rules_;
  std::unordered_map<std::string, Cached> cache_;
  std::unordered_map<Digest128, bool, ws::DigestHasher> emitted_;
  ws::ObjectMap trees_;
};

// Materializes `root` from `objects` into `dir` (created if needed; must be empty
// unless `force`). Omitted (too large) files are listed in `skipped`.
bool checkout(const ws::ObjectMap& objects, const Digest128& root, const std::string& dir, bool force,
              std::vector<std::string>* skipped, std::string* error);

// Snapshots the workspace at each request, records objects/snapshots when a
// writer is given, and in replay compares against the recorded workspace.
class WorkspaceTracker {
public:
  WorkspaceTracker(Snapshotter& snap, RecordingWriter* writer, const rec::Recording* recorded, bool quiet);

  // `recorded_seq`: the recorded request this live request was matched to (replay).
  void before_request(std::uint64_t live_seq, const std::uint64_t* recorded_seq);
  void at_start();
  void at_end(bool compare = true);  // forks skip it: past the fork point, differing is the point
  [[nodiscard]] std::uint64_t divergences() const { return divergences_.load(); }
  [[nodiscard]] std::uint64_t snapshots() const { return snapshots_.load(); }

private:
  void take(ws::When when, std::uint64_t seq, const ws::Snapshot* expected, const std::string& label);

  std::mutex mu_;
  Snapshotter& snap_;
  RecordingWriter* writer_;
  const rec::Recording* recorded_;
  bool quiet_;
  std::atomic<std::uint64_t> divergences_{0}, snapshots_{0};  // read by the status endpoint
};

[[nodiscard]] std::string describe_changes(const std::vector<ws::FileChange>& changes, std::size_t max = 6);

}  // namespace khonsu::native
