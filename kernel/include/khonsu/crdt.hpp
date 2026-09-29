// Khonsu — conflict-free replicated state for shared recording stores.
//
// Teams sync recordings between laptops, CI and a shared store with no
// coordinator and no locks. Two pieces of state are replicated:
//
//  * refs         name -> recording manifest. A last-writer-wins map ordered by
//                 hybrid logical clocks (HLC), ties broken by node id, with
//                 tombstones for deletes.
//  * annotations  notes attached to steps of a recording ("root cause here").
//                 An observed-remove set (OR-Set): every add carries a unique tag,
//                 a remove deletes only the tags it has observed, so a concurrent
//                 add is never lost (unlike an LWW set).
//
// merge() is a join on a semilattice: commutative, associative, idempotent.
// Replicas that have seen the same updates converge to byte-identical encodings
// regardless of delivery order (property-tested).
#pragma once

#include "khonsu/common.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace khonsu::crdt {

// Hybrid logical clock: physical ms + logical counter, so causally later events
// always compare greater even with skewed clocks.
struct Hlc {
  std::uint64_t ms = 0;
  std::uint32_t counter = 0;
  std::uint64_t node = 0;
  friend auto operator<=>(const Hlc&, const Hlc&) = default;
};

class Clock {
public:
  explicit Clock(std::uint64_t node) : node_(node) {}
  Hlc now(std::uint64_t wall_ms);          // local event
  void observe(const Hlc& remote);         // merge a remote timestamp
  [[nodiscard]] std::uint64_t node() const { return node_; }

private:
  std::uint64_t node_;
  Hlc last_{};
};

struct Ref {
  std::string manifest;  // hex id of the recording manifest ("" when deleted)
  Hlc hlc;
  bool deleted = false;
  std::uint64_t size = 0;  // recording bytes, for listings
  friend bool operator==(const Ref&, const Ref&) = default;
};

struct Annotation {
  std::string recording;  // ref name
  std::int64_t step = -1; // call seq it refers to, -1 = whole run
  std::string label;      // e.g. "bug", "root-cause", "ok"
  std::string text;
  std::string author;
  Hlc hlc;
  friend bool operator==(const Annotation&, const Annotation&) = default;
};

// A unique tag per add: (node, per-node counter).
struct Tag {
  std::uint64_t node = 0;
  std::uint64_t counter = 0;
  friend auto operator<=>(const Tag&, const Tag&) = default;
  [[nodiscard]] std::string str() const;
};

class State {
public:
  // ---- refs (LWW map) ----
  void put_ref(const std::string& name, Ref r);  // applies LWW locally
  [[nodiscard]] const Ref* ref(const std::string& name) const;  // nullptr if absent or deleted
  [[nodiscard]] const std::map<std::string, Ref>& refs() const { return refs_; }

  // ---- annotations (OR-Set) ----
  Tag add_annotation(Annotation a, std::uint64_t node);  // returns the new tag
  bool remove_annotation(const Tag& t);                  // removes an observed tag
  [[nodiscard]] std::vector<std::pair<Tag, Annotation>> annotations(const std::string& recording = "") const;

  void merge(const State& other);  // join: commutative, associative, idempotent

  [[nodiscard]] std::vector<std::byte> encode() const;  // canonical bytes
  static std::optional<State> decode(std::span<const std::byte> bytes);
  [[nodiscard]] Digest128 digest() const;
  [[nodiscard]] Hlc max_hlc() const;

private:
  std::map<std::string, Ref> refs_;
  std::map<Tag, Annotation> added_;
  std::set<Tag> removed_;
  std::map<std::uint64_t, std::uint64_t> counters_;  // node -> highest tag counter seen
};

}  // namespace khonsu::crdt
