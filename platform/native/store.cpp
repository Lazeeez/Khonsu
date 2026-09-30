#include "store.hpp"

#include "file_log.hpp"
#include "http.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

namespace khonsu::native {

namespace {

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

bool mkdir_p(const std::string& p) {
  if (p.empty()) return true;
  struct stat st {};
  if (::stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  const auto slash = p.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !mkdir_p(p.substr(0, slash))) return false;
  return ::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

// Durable atomic replace: write a temp file, fdatasync, rename over the target.
bool write_atomic(const std::string& path, std::span<const std::byte> bytes, std::string* error) {
  const std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." +
                          std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (error) *error = tmp + ": " + std::strerror(errno);
    return false;
  }
  std::size_t off = 0;
  while (off < bytes.size()) {
    const ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      if (error) *error = tmp + ": " + std::strerror(errno);
      return false;
    }
    off += static_cast<std::size_t>(w);
  }
  if (::fdatasync(fd) != 0) {
    const int e = errno;
    ::close(fd);
    ::unlink(tmp.c_str());
    if (error) *error = tmp + ": " + std::strerror(e);
    return false;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    if (error) *error = path + ": " + std::strerror(errno);
    return false;
  }
  // The rename is durable only once its directory is: without this, a crash can keep a ref
  // (written later) while losing the object it points at.
  const auto slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  if (const int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); d >= 0) {
    ::fsync(d);
    ::close(d);
  }
  return true;
}

// Compares secrets in time independent of where they differ.
bool same_secret(std::string_view a, std::string_view b) {
  if (b.empty()) return false;
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i % b.size()]);
  return diff == 0;
}

std::string hex2(std::uint8_t b) {
  static constexpr char k[] = "0123456789abcdef";
  return {k[b >> 4], k[b & 15]};
}

void put_summary(Writer& w, const pack::Summary& s) {
  w.varint(s.count).digest(s.root);
  for (const auto& b : s.buckets) w.digest(b);
}

std::optional<pack::Summary> get_summary(std::span<const std::byte> bytes) {
  Reader r(bytes);
  pack::Summary s;
  s.count = static_cast<std::size_t>(r.varint());
  s.root = r.digest();
  for (auto& b : s.buckets) b = r.digest();
  if (!r.ok()) return std::nullopt;
  return s;
}

std::string as_string(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }
std::vector<std::byte> as_bytes_vec(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

}  // namespace

// =========================================================================================
// Store
// =========================================================================================

bool Store::open(const std::string& dir, std::string* error) {
  dir_ = dir;
  if (!mkdir_p(dir_ + "/objects")) {
    if (error) *error = dir_ + ": cannot create store: " + std::strerror(errno);
    return false;
  }
  std::vector<std::byte> id;
  std::string err;
  if (read_file(dir_ + "/node", id, &err) && id.size() == 8) {
    std::memcpy(&node_, id.data(), 8);
  } else {
    // A random 64-bit replica id: breaks HLC ties and makes annotation tags unique.
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f || std::fread(&node_, 8, 1, f) != 1) node_ = static_cast<std::uint64_t>(now_ms()) ^ ::getpid();
    if (f) std::fclose(f);
    node_ |= 1;
    std::vector<std::byte> b(8);
    std::memcpy(b.data(), &node_, 8);
    if (!write_atomic(dir_ + "/node", b, error)) return false;
  }
  return true;
}

std::string Store::path_of(const Digest128& id) const {
  const std::string h = id.hex();
  return dir_ + "/objects/" + h.substr(0, 2) + "/" + h;
}

bool Store::has(const Digest128& id) const {
  struct stat st {};
  return ::stat(path_of(id).c_str(), &st) == 0;
}

std::optional<std::vector<std::byte>> Store::get(const Digest128& id) const {
  std::vector<std::byte> b;
  if (!read_file(path_of(id), b, nullptr)) return std::nullopt;
  if (!pack::verify(id, b)) return std::nullopt;  // bit-rot on disk is never served onward
  return b;
}

bool Store::put(const Digest128& id, std::span<const std::byte> bytes, std::string* error) {
  if (!pack::verify(id, bytes)) {
    if (error) *error = "object " + id.hex() + " failed verification (corrupt or forged)";
    return false;
  }
  if (has(id)) return true;
  const std::string p = path_of(id);
  if (!mkdir_p(p.substr(0, p.find_last_of('/')))) {
    if (error) *error = p + ": " + std::strerror(errno);
    return false;
  }
  return write_atomic(p, bytes, error);
}

std::vector<Digest128> Store::bucket(std::uint8_t b) const {
  std::vector<Digest128> out;
  if (DIR* d = ::opendir((dir_ + "/objects/" + hex2(b)).c_str())) {
    while (dirent* e = ::readdir(d)) {
      bool ok = false;
      const Digest128 id = pack::parse_hex(e->d_name, &ok);
      if (ok) out.push_back(id);
    }
    ::closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

pack::Summary Store::summary() const {
  std::vector<Digest128> all;
  for (unsigned b = 0; b < 256; ++b) {
    auto ids = bucket(static_cast<std::uint8_t>(b));
    all.insert(all.end(), ids.begin(), ids.end());
  }
  return pack::summarize(std::move(all));
}

crdt::State Store::state() const {
  std::vector<std::byte> b;
  if (!read_file(dir_ + "/state", b, nullptr)) return {};
  return crdt::State::decode(b).value_or(crdt::State{});
}

bool Store::save_state(const crdt::State& s, std::string* error) {
  return write_atomic(dir_ + "/state", s.encode(), error);
}

bool Store::update_state(const std::function<void(crdt::State&, crdt::Clock&)>& fn, std::string* error) {
  const int lock = ::open((dir_ + "/state.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock < 0 || ::flock(lock, LOCK_EX) != 0) {
    if (lock >= 0) ::close(lock);
    if (error) *error = "cannot lock " + dir_ + "/state.lock";
    return false;
  }
  crdt::State s = state();
  crdt::Clock clock(node_);
  clock.observe(s.max_hlc());
  fn(s, clock);
  const bool ok = save_state(s, error);
  ::flock(lock, LOCK_UN);
  ::close(lock);
  return ok;
}

bool Store::add_recording(const std::string& name, std::span<const std::byte> bytes, std::string* error,
                          std::size_t* new_objects) {
  const auto p = pack::pack(bytes, error);
  if (!p) return false;
  std::size_t fresh = 0;
  for (const auto& o : p->objects) {
    if (has(o.id)) continue;
    if (!put(o.id, o.bytes, error)) return false;
    ++fresh;
  }
  if (new_objects) *new_objects = fresh;
  // Objects first, ref last: a ref never points at something that isn't stored.
  return update_state([&](crdt::State& s, crdt::Clock& c) {
    s.put_ref(name, crdt::Ref{p->manifest_id.hex(), c.now(now_ms()), false, p->recording_bytes});
  }, error);
}

std::optional<std::vector<std::byte>> Store::export_recording(const std::string& name, std::string* error) const {
  const crdt::State s = state();
  const crdt::Ref* r = s.ref(name);
  if (!r) {
    if (error) *error = "no recording named " + name;
    return std::nullopt;
  }
  bool ok = false;
  const Digest128 m = pack::parse_hex(r->manifest, &ok);
  if (!ok) {
    if (error) *error = "bad manifest id for " + name;
    return std::nullopt;
  }
  return pack::unpack(m, [&](const Digest128& id) { return get(id); }, error);
}

// =========================================================================================
// Remotes
// =========================================================================================

namespace {

class DirRemote final : public Remote {
public:
  bool open(const std::string& dir, std::string* error) { return store_.open(dir, error); }
  std::optional<pack::Summary> summary(std::string*) override {
    ++rt_;
    return store_.summary();
  }
  std::optional<std::vector<std::vector<Digest128>>> buckets(const std::vector<std::uint8_t>& which, std::string*) override {
    ++rt_;
    std::vector<std::vector<Digest128>> out;
    for (auto b : which) out.push_back(store_.bucket(b));
    return out;
  }
  std::optional<std::vector<std::byte>> get(const Digest128& id, std::string* error) override {
    ++rt_;
    auto b = store_.get(id);
    if (!b && error) *error = "remote object " + id.hex() + " missing or corrupt";
    return b;
  }
  bool put(const Digest128& id, std::span<const std::byte> bytes, std::string* error) override {
    ++rt_;
    return store_.put(id, bytes, error);
  }
  std::optional<crdt::State> merge_state(const crdt::State& local, std::string* error) override {
    ++rt_;
    crdt::State merged;
    if (!store_.update_state([&](crdt::State& s, crdt::Clock&) {
          s.merge(local);
          merged = s;
        }, error))
      return std::nullopt;
    return merged;
  }
  [[nodiscard]] std::uint64_t round_trips() const override { return rt_; }

private:
  Store store_;
  std::uint64_t rt_ = 0;
};

class HttpRemote final : public Remote {
public:
  HttpRemote(Endpoint e, std::string token) : ep_(std::move(e)), token_(std::move(token)) {}

  std::optional<pack::Summary> summary(std::string* error) override {
    auto r = call("GET", "/v1/summary", {}, error);
    if (!r) return std::nullopt;
    return get_summary(as_bytes_vec(*r));
  }
  std::optional<std::vector<std::vector<Digest128>>> buckets(const std::vector<std::uint8_t>& which,
                                                              std::string* error) override {
    std::string body(which.size(), '\0');
    for (std::size_t i = 0; i < which.size(); ++i) body[i] = static_cast<char>(which[i]);
    auto r = call("POST", "/v1/buckets", body, error);
    if (!r) return std::nullopt;
    const auto bytes = as_bytes_vec(*r);
    Reader rd(bytes);
    std::vector<std::vector<Digest128>> out(which.size());
    for (auto& v : out) {
      const auto n = rd.varint();
      for (std::uint64_t i = 0; i < n && rd.ok(); ++i) v.push_back(rd.digest());
    }
    if (!rd.ok()) {
      if (error) *error = "malformed bucket listing from remote";
      return std::nullopt;
    }
    return out;
  }
  std::optional<std::vector<std::byte>> get(const Digest128& id, std::string* error) override {
    auto r = call("GET", "/v1/objects/" + id.hex(), {}, error);
    if (!r) return std::nullopt;
    return as_bytes_vec(*r);
  }
  bool put(const Digest128& id, std::span<const std::byte> bytes, std::string* error) override {
    return call("PUT", "/v1/objects/" + id.hex(), as_string(bytes), error).has_value();
  }
  std::optional<crdt::State> merge_state(const crdt::State& local, std::string* error) override {
    auto r = call("POST", "/v1/state", as_string(local.encode()), error);
    if (!r) return std::nullopt;
    auto s = crdt::State::decode(as_bytes_vec(*r));
    if (!s && error) *error = "malformed state from remote";
    return s;
  }
  [[nodiscard]] std::uint64_t round_trips() const override { return rt_; }

private:
  std::optional<std::string> call(const std::string& method, const std::string& path, const std::string& body,
                                  std::string* error) {
    ++rt_;
    auto s = connect(ep_, 60'000, error);
    if (!s) return std::nullopt;
    const bool default_port = (ep_.tls && ep_.port == 443) || (!ep_.tls && ep_.port == 80);
    std::string req = method + " " + ep_.base_path + path + " HTTP/1.1\r\nHost: " + ep_.host +
                      (default_port ? "" : ":" + std::to_string(ep_.port)) + "\r\n";
    if (!token_.empty()) req += "Authorization: Bearer " + token_ + "\r\n";
    req += "Content-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n";
    if (!s->write_all(req) || !s->write_all(body)) {
      if (error) *error = "failed to send to " + to_string(ep_);
      return std::nullopt;
    }
    BufReader in(*s);
    ResponseHead head;
    if (!read_response_head(in, head, error)) return std::nullopt;
    std::string out;
    BodyReader br(in, head.headers);
    while (br.next(out)) {
    }
    if (head.status != 200 && head.status != 201) {
      if (error) *error = "remote " + method + " " + path + ": HTTP " + std::to_string(head.status) + " " + out;
      return std::nullopt;
    }
    return out;
  }
  Endpoint ep_;
  std::string token_;
  std::uint64_t rt_ = 0;
};

}  // namespace

std::unique_ptr<Remote> open_remote(const std::string& spec, const std::string& token, std::string* error) {
  if (spec.rfind("http://", 0) == 0 || spec.rfind("https://", 0) == 0) {
    Endpoint e;
    if (!parse_url(spec, e, error)) return nullptr;
    return std::make_unique<HttpRemote>(e, token);
  }
  auto r = std::make_unique<DirRemote>();
  if (!r->open(spec, error)) return nullptr;
  return r;
}

// =========================================================================================
// Anti-entropy
// =========================================================================================

bool sync(Store& local, Remote& remote, bool push, bool pull, SyncStats& st, std::string* error) {
  const crdt::State before = local.state();
  st.refs_before = before.refs().size();

  const auto rs = remote.summary(error);
  if (!rs) return false;
  const pack::Summary ls = local.summary();
  st.roots_equal = ls.root == rs->root;

  if (!st.roots_equal) {
    std::vector<std::uint8_t> differing;
    for (unsigned b = 0; b < 256; ++b)
      if (!(ls.buckets[b] == rs->buckets[b])) differing.push_back(static_cast<std::uint8_t>(b));
    st.buckets_differing = differing.size();
    const auto remote_lists = remote.buckets(differing, error);
    if (!remote_lists) return false;
    for (std::size_t k = 0; k < differing.size(); ++k) {
      const auto mine = local.bucket(differing[k]);
      const auto& theirs = (*remote_lists)[k];
      st.ids_listed += mine.size() + theirs.size();
      std::vector<Digest128> only_mine, only_theirs;
      std::set_difference(mine.begin(), mine.end(), theirs.begin(), theirs.end(), std::back_inserter(only_mine));
      std::set_difference(theirs.begin(), theirs.end(), mine.begin(), mine.end(), std::back_inserter(only_theirs));
      if (push)
        for (const auto& id : only_mine) {
          const auto b = local.get(id);
          if (!b) continue;  // corrupt locally: never propagate it
          if (!remote.put(id, *b, error)) return false;
          ++st.sent_objects;
          st.sent_bytes += b->size();
        }
      if (pull)
        for (const auto& id : only_theirs) {
          const auto b = remote.get(id, error);
          if (!b) return false;
          if (!local.put(id, *b, error)) return false;  // verifies the id: a corrupt remote object stops here
          ++st.received_objects;
          st.received_bytes += b->size();
        }
    }
  }

  // State last, so every ref the other side learns about points at objects it already has.
  const auto merged = remote.merge_state(push ? before : crdt::State{}, error);
  if (!merged) return false;
  if (pull &&
      !local.update_state([&](crdt::State& s, crdt::Clock& c) {
        s.merge(*merged);
        c.observe(s.max_hlc());
      }, error))
    return false;
  const crdt::State after = local.state();
  st.refs_after = after.refs().size();
  st.annotations_after = after.annotations().size();
  st.round_trips = remote.round_trips();
  return true;
}

// =========================================================================================
// HTTP server for a store
// =========================================================================================

int serve_store(Store& store, const std::string& bind, std::uint16_t port, const std::string& token,
                const std::atomic<bool>& stop, bool quiet) {
  ignore_sigpipe();
  Listener l;
  std::string err;
  if (!l.listen(port, &err, bind)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  std::printf("khonsu store serving %s on http://%s:%u%s\n", store.dir().c_str(), bind.c_str(), l.port(),
              token.empty() ? "" : " (token required)");
  std::fflush(stdout);
  std::atomic<int> active{0};
  std::thread acceptor([&] {
    while (auto s = l.accept()) {
      active.fetch_add(1);
      std::thread([&, conn = std::move(s)]() mutable {
        Stream& c = *conn;
        BufReader in(c);
        HttpRequest req;
        std::string e;
        auto reply = [&](int status, std::string_view body, std::string_view type = "application/octet-stream") {
          ResponseWriter w(c);
          (void)(w.head(status, type, {}, false, body.size()) && w.chunk(body) && w.finish());
        };
        if (read_request(in, req, &e)) {
          const std::string_view auth = req.headers.get("authorization");
          if (!token.empty() && !same_secret(auth, "Bearer " + token)) {
            reply(401, "unauthorized", "text/plain");
          } else if (req.method == "GET" && req.target == "/v1/summary") {
            Writer w;
            put_summary(w, store.summary());
            reply(200, as_string(w.span()));
          } else if (req.method == "POST" && req.target == "/v1/buckets") {
            Writer w;
            for (const char b : req.body) {
              const auto ids = store.bucket(static_cast<std::uint8_t>(b));
              w.varint(ids.size());
              for (const auto& id : ids) w.digest(id);
            }
            reply(200, as_string(w.span()));
          } else if (req.target.rfind("/v1/objects/", 0) == 0) {
            bool ok = false;
            const Digest128 id = pack::parse_hex(std::string_view(req.target).substr(12), &ok);
            if (!ok) {
              reply(400, "bad object id", "text/plain");
            } else if (req.method == "GET") {
              if (auto b = store.get(id)) reply(200, as_string(*b));
              else reply(404, "not found", "text/plain");
            } else if (req.method == "PUT") {
              if (store.put(id, as_bytes_vec(req.body), &e)) reply(201, "");
              else reply(400, e, "text/plain");
            } else {
              reply(405, "method not allowed", "text/plain");
            }
          } else if (req.method == "POST" && req.target == "/v1/state") {
            auto incoming = crdt::State::decode(as_bytes_vec(req.body));
            crdt::State merged;
            if (!incoming) {
              reply(400, "bad state", "text/plain");
            } else if (store.update_state([&](crdt::State& s, crdt::Clock&) {
                         s.merge(*incoming);
                         merged = s;
                       }, &e)) {
              reply(200, as_string(merged.encode()));
            } else {
              reply(500, e, "text/plain");
            }
          } else {
            reply(404, "unknown endpoint", "text/plain");
          }
          if (!quiet) std::fprintf(stderr, "[khonsu store] %s %s\n", req.method.c_str(), req.target.c_str());
        }
        active.fetch_sub(1);
      }).detach();
    }
  });
  while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  l.shutdown();
  acceptor.join();
  l.close();
  while (active.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return 0;
}

}  // namespace khonsu::native
