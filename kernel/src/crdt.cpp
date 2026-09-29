#include "khonsu/crdt.hpp"

#include "khonsu/log.hpp"

#include <algorithm>
#include <cstdio>

namespace khonsu::crdt {

namespace {
constexpr std::uint64_t kStateDomain = 0x4b484e2d43524454ULL;  // "KHN-CRDT"

void put_hlc(Writer& w, const Hlc& h) { w.varint(h.ms).varint(h.counter).varint(h.node); }
Hlc get_hlc(Reader& r) {
  Hlc h;
  h.ms = r.varint();
  h.counter = static_cast<std::uint32_t>(r.varint());
  h.node = r.varint();
  return h;
}
}  // namespace

Hlc Clock::now(std::uint64_t wall_ms) {
  if (wall_ms > last_.ms) {
    last_ = {wall_ms, 0, node_};
  } else {
    last_ = {last_.ms, last_.counter + 1, node_};
  }
  return last_;
}

void Clock::observe(const Hlc& remote) {
  if (remote.ms > last_.ms || (remote.ms == last_.ms && remote.counter > last_.counter))
    last_ = {remote.ms, remote.counter, node_};
}

std::string Tag::str() const {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%llx.%llu", static_cast<unsigned long long>(node),
                static_cast<unsigned long long>(counter));
  return buf;
}

void State::put_ref(const std::string& name, Ref r) {
  auto it = refs_.find(name);
  if (it == refs_.end() || it->second.hlc < r.hlc) refs_[name] = std::move(r);
}

const Ref* State::ref(const std::string& name) const {
  const auto it = refs_.find(name);
  return it == refs_.end() || it->second.deleted ? nullptr : &it->second;
}

Tag State::add_annotation(Annotation a, std::uint64_t node) {
  Tag t{node, ++counters_[node]};
  added_.emplace(t, std::move(a));
  return t;
}

bool State::remove_annotation(const Tag& t) {
  if (!added_.contains(t) || removed_.contains(t)) return false;
  removed_.insert(t);
  return true;
}

std::vector<std::pair<Tag, Annotation>> State::annotations(const std::string& recording) const {
  std::vector<std::pair<Tag, Annotation>> out;
  for (const auto& [t, a] : added_)
    if (!removed_.contains(t) && (recording.empty() || a.recording == recording)) out.emplace_back(t, a);
  std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) {
    return x.second.step != y.second.step ? x.second.step < y.second.step : x.second.hlc < y.second.hlc;
  });
  return out;
}

void State::merge(const State& other) {
  for (const auto& [name, r] : other.refs_) put_ref(name, r);
  // Tags are unique, so the add-set union needs no conflict resolution.
  for (const auto& [t, a] : other.added_) added_.emplace(t, a);
  removed_.insert(other.removed_.begin(), other.removed_.end());
  for (const auto& [node, c] : other.counters_) counters_[node] = std::max(counters_[node], c);
}

std::vector<std::byte> State::encode() const {
  Writer w;
  w.str("khonsu-state/1").varint(refs_.size());
  for (const auto& [name, r] : refs_) {
    w.str(name).str(r.manifest).u8(r.deleted ? 1 : 0).varint(r.size);
    put_hlc(w, r.hlc);
  }
  w.varint(added_.size());
  for (const auto& [t, a] : added_) {
    w.varint(t.node).varint(t.counter).str(a.recording);
    w.varint(static_cast<std::uint64_t>(a.step + 1)).str(a.label).str(a.text).str(a.author);
    put_hlc(w, a.hlc);
  }
  w.varint(removed_.size());
  for (const auto& t : removed_) w.varint(t.node).varint(t.counter);
  w.varint(counters_.size());
  for (const auto& [node, c] : counters_) w.varint(node).varint(c);
  return w.bytes();
}

std::optional<State> State::decode(std::span<const std::byte> bytes) {
  Reader r(bytes);
  if (r.str() != "khonsu-state/1") return std::nullopt;
  State s;
  const auto nrefs = r.varint();
  for (std::uint64_t i = 0; i < nrefs && r.ok(); ++i) {
    std::string name(r.str());
    Ref ref;
    ref.manifest = std::string(r.str());
    ref.deleted = r.u8() != 0;
    ref.size = r.varint();
    ref.hlc = get_hlc(r);
    s.refs_[name] = ref;
  }
  const auto nadd = r.varint();
  for (std::uint64_t i = 0; i < nadd && r.ok(); ++i) {
    Tag t;
    t.node = r.varint();
    t.counter = r.varint();
    Annotation a;
    a.recording = std::string(r.str());
    a.step = static_cast<std::int64_t>(r.varint()) - 1;
    a.label = std::string(r.str());
    a.text = std::string(r.str());
    a.author = std::string(r.str());
    a.hlc = get_hlc(r);
    s.added_.emplace(t, std::move(a));
  }
  const auto nrem = r.varint();
  for (std::uint64_t i = 0; i < nrem && r.ok(); ++i) {
    Tag t;
    t.node = r.varint();
    t.counter = r.varint();
    s.removed_.insert(t);
  }
  const auto nc = r.varint();
  for (std::uint64_t i = 0; i < nc && r.ok(); ++i) {
    const auto node = r.varint();
    s.counters_[node] = r.varint();
  }
  if (!r.ok()) return std::nullopt;
  return s;
}

Digest128 State::digest() const { return digest_bytes(encode(), kStateDomain); }

Hlc State::max_hlc() const {
  Hlc m;
  for (const auto& [n, r] : refs_) m = std::max(m, r.hlc);
  for (const auto& [t, a] : added_) m = std::max(m, a.hlc);
  return m;
}

}  // namespace khonsu::crdt
