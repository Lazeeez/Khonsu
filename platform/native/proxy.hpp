// Khonsu — the recording/replaying model-API proxy.
//
// Agents talk to it exactly as they would to an OpenAI-compatible endpoint
// (OPENAI_BASE_URL=http://127.0.0.1:<port>/v1). Depending on the mode it:
//   record   forwards to the upstream and writes every exchange to a recording;
//   replay   serves recorded responses by (match key, occurrence), zero network;
//            a request that is not in the recording is a divergence, which either
//            fails loudly (--on-miss=fail) or is forwarded live (--on-miss=live,
//            "replay-then-live": pay only for what changed).
#pragma once

#include "file_log.hpp"
#include "net.hpp"
#include "workspace.hpp"

#include "khonsu/recording.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace khonsu::native {

// Appends recording frames to a FileLog while maintaining the RunEnd hash chain.
class RecordingWriter {
public:
  explicit RecordingWriter(FileLog& log) : log_(log) {}
  void run_start(const rec::RunInfo& info);
  void request(const rec::Request& r, std::uint64_t t_us);
  void response(const rec::Response& r, std::uint64_t t_us);  // durable on return (fdatasync)
  void run_end(rec::RunEnd end, std::uint64_t t_us);
  void object(ws::ObjectKind kind, const Digest128& id, std::span<const std::byte> bytes);  // not synced
  void snapshot(const ws::Snapshot& s);                                                   // not synced
  [[nodiscard]] std::uint64_t exchanges() const { return exchanges_.load(); }
  [[nodiscard]] FileLog& log() { return log_; }

private:
  void append(FrameType t, std::uint16_t actor, std::uint64_t t_us, const std::vector<std::byte>& payload, bool sync);
  FileLog& log_;
  std::mutex mu_;
  Digest128 chain_;
  std::atomic<std::uint64_t> exchanges_{0};
};

enum class Mode { Record, Replay };
enum class OnMiss { Fail, Live };

struct ProxyConfig {
  Mode mode = Mode::Record;
  OnMiss on_miss = OnMiss::Fail;
  std::optional<Endpoint> upstream;  // required for Record and for OnMiss::Live
  std::string route = "/v1";         // requests under this prefix are handled
  bool realtime = false;             // replay streams with their recorded timing
  bool quiet = false;
  int upstream_timeout_ms = 600'000;
  RecordingWriter* writer = nullptr;  // where exchanges are recorded (optional in Replay)
  const rec::Recording* replay = nullptr;
  std::uint64_t live_from = UINT64_MAX;  // replay: recorded exchanges >= this seq go live (fork)
  WorkspaceTracker* workspace = nullptr;  // snapshot/compare the workspace before each request
};

struct Divergence {
  std::uint64_t seq = 0;
  std::string path;
  rec::ReplayIndex::Miss miss;
};

struct ProxyStats {
  std::atomic<std::uint64_t> requests{0}, recorded{0}, replayed{0}, live{0}, divergences{0}, errors{0};
  std::atomic<std::uint64_t> tools_replayed{0}, tools_executed{0}, forked{0};
};

struct DigestHasherN {
  std::size_t operator()(const Digest128& d) const noexcept { return static_cast<std::size_t>(d.lo ^ d.hi); }
};

struct Incoming;  // one parsed client request (proxy.cpp)

class Proxy {
public:
  explicit Proxy(ProxyConfig cfg);
  ~Proxy();

  bool start(std::uint16_t port, std::string* error);
  void stop();  // stops accepting and waits for in-flight exchanges
  [[nodiscard]] std::uint16_t port() const { return listener_.port(); }
  [[nodiscard]] const ProxyStats& stats() const { return stats_; }
  [[nodiscard]] std::vector<Divergence> divergences() const;
  [[nodiscard]] std::string status_json() const;  // served at GET /khonsu/v1/status
  [[nodiscard]] std::uint64_t elapsed_us() const;

private:
  void accept_loop();
  void handle(std::unique_ptr<Stream> s);
  void serve_recorded(Stream& client, const rec::Exchange& ex);
  void forward_live(Stream& client, const Incoming& in, bool after_miss);
  void handle_tool(Stream& client, Incoming& in, std::string_view action);
  void identify(Incoming& in, std::string method, std::string path, std::string canonical);
  void record_copy(const Incoming& in, const rec::Exchange& ex);
  // Returns the recorded exchange, nullptr if the request must go live, or reports
  // a divergence (writes the 409) and sets `refused`.
  const rec::Exchange* match(Stream& client, Incoming& in, bool& refused, bool& after_miss);
  void log(const std::string& line) const;

  ProxyConfig cfg_;
  Listener listener_;
  std::thread acceptor_;
  std::unique_ptr<rec::ReplayIndex> index_;
  std::chrono::steady_clock::time_point t0_;
  ProxyStats stats_;
  std::atomic<std::uint64_t> seq_{0};
  mutable std::mutex mu_;
  std::unordered_map<Digest128, std::uint32_t, DigestHasherN> occurrences_;
  std::vector<Divergence> divergences_;
  std::unordered_map<std::uint64_t, std::unique_ptr<Incoming>> pending_tools_;  // token -> begun tool call
  std::condition_variable idle_;
  std::size_t active_ = 0;
  bool stopping_ = false;
};


}  // namespace khonsu::native
