// Khonsu — recordings of real agent runs.
//
// A recording is an EventLog (CRC32C frames) with one frame per run boundary and
// one per HTTP request/response the agent made to a model API:
//
//   RunStart · (HttpRequest HttpResponse)* · RunEnd
//
// Requests are identified by a *match key* = H(method, path, canonical JSON body),
// plus an occurrence number (the n-th time this exact request appeared). Replay
// serves the recorded response for (key, occurrence); a request whose key was
// never recorded is a *divergence*, reported with the path of the first JSON
// field that differs from the request the recording expected next.
//
// This file is portable (native CLI and the WebAssembly viewer share it).
#pragma once

#include "khonsu/common.hpp"
#include "khonsu/jsonv.hpp"
#include "khonsu/log.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace khonsu::rec {

inline constexpr std::string_view kFormatVersion = "khonsu-recording/1";

struct RunInfo {
  std::string format{kFormatVersion};
  std::string tool_version;
  std::string mode;      // "record" | "replay" | "replay-then-live"
  std::string command;   // human-readable command line
  std::string cwd;
  std::string upstream;  // e.g. http://localhost:11434/v1
  std::string parent;    // digest of the recording this one forked from ("" if none)
  std::uint64_t started_unix_ms = 0;
};

struct Request {
  std::uint64_t seq = 0;         // arrival order within the run
  Digest128 key;                 // match key
  std::uint32_t occurrence = 0;  // n-th arrival of this key
  std::uint64_t t_us = 0;        // arrival time since run start
  std::string method, path;
  std::string headers_json;      // allow-listed, secrets redacted
  std::string body;              // canonical JSON (or raw bytes if not JSON)
};

struct Chunk {
  std::uint32_t end = 0;   // body offset where this chunk ends
  std::uint64_t t_us = 0;  // time since the request arrived
};

struct Response {
  std::uint64_t seq = 0;
  std::uint32_t status = 0;
  std::string content_type;
  std::uint64_t latency_us = 0;  // request arrival → last byte
  bool streamed = false;         // body was a server-sent-event stream
  bool from_recording = false;   // served by replay rather than the upstream
  std::string body;
  std::vector<Chunk> chunks;     // stream chunk boundaries and timing
};

struct RunEnd {
  std::int64_t exit_code = 0;
  std::uint64_t duration_us = 0;
  std::uint64_t exchanges = 0;
  Digest128 chain;  // hash chain over every frame CRC before this one
};

// ---- encoding ---------------------------------------------------------------------
[[nodiscard]] std::vector<std::byte> encode(const RunInfo& v);
[[nodiscard]] std::vector<std::byte> encode(const Request& v);
[[nodiscard]] std::vector<std::byte> encode(const Response& v);
[[nodiscard]] std::vector<std::byte> encode(const RunEnd& v);
[[nodiscard]] std::optional<RunInfo> decode_run(std::span<const std::byte> p);
[[nodiscard]] std::optional<Request> decode_request(std::span<const std::byte> p);
[[nodiscard]] std::optional<Response> decode_response(std::span<const std::byte> p);
[[nodiscard]] std::optional<RunEnd> decode_end(std::span<const std::byte> p);

// Canonical body (canonical JSON if parseable, otherwise the raw bytes) and match key.
[[nodiscard]] std::string canonical_body(std::string_view body);
[[nodiscard]] Digest128 match_key(std::string_view method, std::string_view path, std::string_view canonical);

// Running hash chain over frame CRCs (what RunEnd.chain commits to).
[[nodiscard]] Digest128 chain_step(const Digest128& prev, std::uint32_t crc);

// ---- reading ------------------------------------------------------------------------
struct Exchange {
  Request req;
  std::optional<Response> resp;  // absent if the run crashed mid-request
};

struct Recording {
  RunInfo run;
  std::vector<Exchange> exchanges;  // ordered by seq
  std::optional<RunEnd> end;        // absent if the run crashed
  RecoveryReport recovery;          // torn tail / corruption found while loading
  bool chain_ok = true;             // RunEnd.chain matched (true if no RunEnd)
  Digest128 digest;                 // identity of the recording (chain at load)

  // Loads a serialized EventLog. Frames after the first corrupt one are dropped
  // (WAL-style recovery). Returns nullopt only if the file is not a recording.
  static std::optional<Recording> load(std::span<const std::byte> bytes, std::string* error = nullptr);
};

// Summary fields pulled from OpenAI-compatible bodies (for `khonsu show` and the viewer).
struct ExchangeSummary {
  std::string model;
  std::string last_user_message;  // truncated
  std::string output_text;        // assistant text (stream deltas joined), truncated
  std::uint64_t prompt_tokens = 0, completion_tokens = 0;
  std::string tool_calls;         // comma-separated tool/function names requested
};
[[nodiscard]] ExchangeSummary summarize(const Exchange& ex, std::size_t max_chars = 160);

// ---- replay matching ------------------------------------------------------------------
class ReplayIndex {
public:
  explicit ReplayIndex(const Recording& r);

  struct Miss {
    std::uint64_t expected_seq = 0;  // the request the recording expected next
    std::string field_path;          // first differing JSON path ("" if not comparable)
    std::string expected_value, actual_value;  // truncated renderings of that field
  };

  // Thread-safe. Returns the recorded exchange for this request, or nullptr and
  // fills `miss` when the request is not in the recording (a divergence).
  const Exchange* take(const Digest128& key, std::string_view canonical_body, Miss* miss);

  [[nodiscard]] std::size_t served() const;
  [[nodiscard]] std::size_t total() const { return rec_.exchanges.size(); }

private:
  const Recording& rec_;
  mutable std::mutex mu_;
  struct DigestHasher {
    std::size_t operator()(const Digest128& d) const noexcept { return static_cast<std::size_t>(d.lo ^ d.hi); }
  };
  std::unordered_map<Digest128, std::vector<std::size_t>, DigestHasher> by_key_;  // key -> exchange indices
  std::unordered_map<Digest128, std::size_t, DigestHasher> next_;                // key -> next occurrence
  std::vector<bool> used_;
  std::size_t served_ = 0;
};

}  // namespace khonsu::rec
