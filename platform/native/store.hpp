// Khonsu — shared recording stores and Merkle anti-entropy sync (POSIX).
//
// A store is a directory:
//   objects/<bucket>/<id>   content-addressed objects (bucket = first byte of the id)
//   state                   replicated refs + annotations (crdt::State), atomically replaced
//   node                    this replica's random node id
//
// sync() makes two stores equal with no coordinator:
//   1. exchange 256-bucket Merkle summaries (1 round trip); equal roots => done;
//   2. for differing buckets only, exchange id lists (1 round trip, batched);
//   3. copy each side's missing objects, verifying every id on receipt;
//   4. merge the CRDT state, *after* objects, so a ref never points at an object
//      the remote doesn't have yet.
// Transports: another store directory (shared drive, CI cache) or HTTP to
// `khonsu store serve` (optionally with a bearer token).
#pragma once

#include "net.hpp"

#include "khonsu/crdt.hpp"
#include "khonsu/pack.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace khonsu::native {

class Store {
public:
  bool open(const std::string& dir, std::string* error);  // creates if missing
  [[nodiscard]] const std::string& dir() const { return dir_; }

  [[nodiscard]] bool has(const Digest128& id) const;
  [[nodiscard]] std::optional<std::vector<std::byte>> get(const Digest128& id) const;
  bool put(const Digest128& id, std::span<const std::byte> bytes, std::string* error);  // verifies the id
  [[nodiscard]] std::vector<Digest128> bucket(std::uint8_t b) const;
  [[nodiscard]] pack::Summary summary() const;

  [[nodiscard]] crdt::State state() const;
  bool save_state(const crdt::State& s, std::string* error);  // atomic replace
  // Read-modify-write of the state under an exclusive file lock (safe across processes).
  bool update_state(const std::function<void(crdt::State&, crdt::Clock&)>& fn, std::string* error);
  [[nodiscard]] std::uint64_t node() const { return node_; }

  // Imports a recording file: packs it and publishes `name` -> manifest.
  bool add_recording(const std::string& name, std::span<const std::byte> bytes, std::string* error,
                     std::size_t* new_objects = nullptr);
  std::optional<std::vector<std::byte>> export_recording(const std::string& name, std::string* error) const;

private:
  [[nodiscard]] std::string path_of(const Digest128& id) const;
  std::string dir_;
  std::uint64_t node_ = 0;
};

// The other side of a sync.
class Remote {
public:
  virtual ~Remote() = default;
  virtual std::optional<pack::Summary> summary(std::string* error) = 0;
  virtual std::optional<std::vector<std::vector<Digest128>>> buckets(const std::vector<std::uint8_t>& which,
                                                                      std::string* error) = 0;
  virtual std::optional<std::vector<std::byte>> get(const Digest128& id, std::string* error) = 0;
  virtual bool put(const Digest128& id, std::span<const std::byte> bytes, std::string* error) = 0;
  // Merges `local` into the remote state and returns the remote's merged state.
  virtual std::optional<crdt::State> merge_state(const crdt::State& local, std::string* error) = 0;
  [[nodiscard]] virtual std::uint64_t round_trips() const = 0;
};

// "http(s)://host:port" -> HTTP remote; anything else -> a store directory.
std::unique_ptr<Remote> open_remote(const std::string& spec, const std::string& token, std::string* error);

struct SyncStats {
  bool roots_equal = false;
  std::size_t buckets_differing = 0, ids_listed = 0;
  std::size_t sent_objects = 0, received_objects = 0;
  std::uint64_t sent_bytes = 0, received_bytes = 0;
  std::size_t refs_before = 0, refs_after = 0, annotations_after = 0;
  std::uint64_t round_trips = 0;
};

bool sync(Store& local, Remote& remote, bool push, bool pull, SyncStats& stats, std::string* error);

// Serves a store over HTTP until `stop` becomes true. Thread per connection.
int serve_store(Store& store, const std::string& bind, std::uint16_t port, const std::string& token,
                const std::atomic<bool>& stop, bool quiet);

}  // namespace khonsu::native
