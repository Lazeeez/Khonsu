// khonsu — time-travel debugger for AI agents.
//
//   khonsu record [-o run.khn] [--upstream URL] -- python agent.py
//   khonsu replay run.khn [--on-miss fail|live] [-o fork.khn] -- python agent.py
//   khonsu serve  (--record OUT | --replay IN) [...]
//   khonsu show   run.khn [--json]
//   khonsu verify run.khn
#include "file_log.hpp"
#include "net.hpp"
#include "process.hpp"
#include "proxy.hpp"

#include "khonsu/json.hpp"
#include "khonsu/recording.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

using namespace khonsu;
using namespace khonsu::native;

namespace {

constexpr const char* kVersion = "0.2.0";

const bool kColor = ::isatty(2);
std::string dim(const std::string& s) { return kColor ? "\033[2m" + s + "\033[0m" : s; }
std::string bold(const std::string& s) { return kColor ? "\033[1m" + s + "\033[0m" : s; }
std::string gold(const std::string& s) { return kColor ? "\033[33m" + s + "\033[0m" : s; }
std::string red(const std::string& s) { return kColor ? "\033[31m" + s + "\033[0m" : s; }
std::string green(const std::string& s) { return kColor ? "\033[32m" + s + "\033[0m" : s; }

int usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "khonsu %s: time-travel debugger for AI agents\n\n"
               "usage:\n"
               "  khonsu record [-o FILE] [--upstream URL] [--port N] [--no-fsync] [-q] -- CMD [ARGS...]\n"
               "      Run CMD with OPENAI_BASE_URL pointed at a recording proxy.\n"
               "  khonsu replay FILE [--on-miss fail|live] [--upstream URL] [-o FILE] [--realtime] [-q] -- CMD...\n"
               "      Run CMD against the recording: zero tokens, zero network.\n"
               "      --on-miss live: requests that differ from the recording go to the upstream\n"
               "      (replay-then-live); -o records the new run.\n"
               "  khonsu serve (--record FILE | --replay FILE) [same options] \n"
               "      Run the proxy until Ctrl-C and print the base URL to use.\n"
               "  khonsu show FILE [--json]      Summarize a recording.\n"
               "  khonsu verify FILE             Check frame CRCs and the hash chain.\n"
               "  khonsu version\n\n"
               "upstream: --upstream, else $KHONSU_UPSTREAM, else $OPENAI_BASE_URL, else https://api.openai.com/v1\n"
               "          (local and free: --upstream http://localhost:11434/v1 for Ollama)\n",
               kVersion);
  return code;
}

struct Options {
  std::string cmd;
  std::string file;      // positional FILE (replay/show/verify)
  std::string out;       // -o
  std::string record_to, replay_from;  // serve
  std::string upstream;
  std::string on_miss = "fail";
  std::uint16_t port = 0;
  bool fsync = true, realtime = false, quiet = false, json = false;
  std::vector<std::string> argv;  // after --
};

bool parse_args(int argc, char** argv, Options& o, std::string& err) {
  if (argc < 2) return false;
  o.cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) {
        err = a + " needs a value";
        return false;
      }
      dst = argv[++i];
      return true;
    };
    if (a == "--") {
      for (++i; i < argc; ++i) o.argv.emplace_back(argv[i]);
      break;
    }
    if (a == "-o" || a == "--out") { if (!value(o.out)) return false; }
    else if (a == "--upstream") { if (!value(o.upstream)) return false; }
    else if (a == "--on-miss") { if (!value(o.on_miss)) return false; }
    else if (a == "--record") { if (!value(o.record_to)) return false; }
    else if (a == "--replay") { if (!value(o.replay_from)) return false; }
    else if (a == "--port") {
      std::string p;
      if (!value(p)) return false;
      o.port = static_cast<std::uint16_t>(std::atoi(p.c_str()));
    }
    else if (a == "--no-fsync") o.fsync = false;
    else if (a == "--realtime") o.realtime = true;
    else if (a == "-q" || a == "--quiet") o.quiet = true;
    else if (a == "--json") o.json = true;
    else if (!a.empty() && a[0] == '-') { err = "unknown option " + a; return false; }
    else if (o.file.empty()) o.file = a;
    else { err = "unexpected argument " + a; return false; }
  }
  if (o.on_miss != "fail" && o.on_miss != "live") {
    err = "--on-miss must be fail or live";
    return false;
  }
  return true;
}

std::string default_run_path() {
  ::mkdir(".khonsu", 0755);
  ::mkdir(".khonsu/runs", 0755);
  char buf[64];
  const std::time_t t = std::time(nullptr);
  std::strftime(buf, sizeof buf, ".khonsu/runs/run-%Y%m%d-%H%M%S.khn", std::localtime(&t));
  return buf;
}

std::string resolve_upstream(const Options& o) {
  if (!o.upstream.empty()) return o.upstream;
  if (const char* e = std::getenv("KHONSU_UPSTREAM"); e && *e) return e;
  if (const char* e = std::getenv("OPENAI_BASE_URL"); e && *e) return e;
  return "https://api.openai.com/v1";
}

std::uint64_t now_unix_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

std::string human_bytes(std::uint64_t b) {
  char buf[32];
  if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  else if (b < (1u << 20)) std::snprintf(buf, sizeof buf, "%.1f KiB", static_cast<double>(b) / 1024);
  else std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(b) / (1 << 20));
  return buf;
}

std::string ms(std::uint64_t us) {
  char buf[32];
  std::snprintf(buf, sizeof buf, us < 10'000'000 ? "%.0f ms" : "%.1f s",
                us < 10'000'000 ? static_cast<double>(us) / 1e3 : static_cast<double>(us) / 1e6);
  return buf;
}

bool load_recording(const std::string& path, rec::Recording& out, bool report_recovery = true) {
  std::vector<std::byte> bytes;
  std::string err;
  if (!read_file(path, bytes, &err)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return false;
  }
  auto r = rec::Recording::load(bytes, &err);
  if (!r) {
    std::fprintf(stderr, "khonsu: %s: %s\n", path.c_str(), err.c_str());
    return false;
  }
  if (report_recovery && !r->recovery.reason.empty())
    std::fprintf(stderr, "%s\n",
                 gold("khonsu: " + path + ": recovered " + std::to_string(r->recovery.frames_recovered) +
                      " frames; discarded " + std::to_string(r->recovery.bytes_discarded) + " bytes (" +
                      r->recovery.reason + ")").c_str());
  out = std::move(*r);
  return true;
}

// Shared by record / replay / serve.
int run_proxy(const Options& o, Mode mode, const std::string& replay_path, const std::string& out_path, bool serve) {
  rec::Recording replay;
  if (mode == Mode::Replay && !load_recording(replay_path, replay)) return 2;

  const bool need_upstream = mode == Mode::Record || o.on_miss == "live";
  std::optional<Endpoint> upstream;
  if (need_upstream) {
    Endpoint e;
    std::string err;
    if (!parse_url(resolve_upstream(o), e, &err)) {
      std::fprintf(stderr, "khonsu: bad upstream URL: %s\n", err.c_str());
      return 2;
    }
    if (e.tls && !tls_available()) {
      std::fprintf(stderr, "khonsu: upstream %s needs TLS, which this build lacks\n", to_string(e).c_str());
      return 2;
    }
    upstream = e;
  }

  FileLog log;
  std::unique_ptr<RecordingWriter> writer;
  if (!out_path.empty()) {
    std::string err;
    RecoveryReport rep;
    if (!log.open(out_path, o.fsync ? SyncPolicy::Always : SyncPolicy::None, &err, &rep)) {
      std::fprintf(stderr, "khonsu: %s\n", err.c_str());
      return 2;
    }
    if (log.frames() > 0) {
      std::fprintf(stderr, "khonsu: %s already contains a recording; choose a new file\n", out_path.c_str());
      return 2;
    }
    writer = std::make_unique<RecordingWriter>(log);
    rec::RunInfo info;
    info.tool_version = kVersion;
    info.mode = mode == Mode::Record ? "record" : (o.on_miss == "live" ? "replay-then-live" : "replay");
    info.command = serve ? "(serve)" : shell_join(o.argv);
    info.cwd = current_dir();
    info.upstream = upstream ? to_string(*upstream) : "";
    info.parent = mode == Mode::Replay ? replay.digest.hex() : "";
    info.started_unix_ms = now_unix_ms();
    writer->run_start(info);
  }

  ProxyConfig cfg;
  cfg.mode = mode;
  cfg.on_miss = o.on_miss == "live" ? OnMiss::Live : OnMiss::Fail;
  cfg.upstream = upstream;
  cfg.realtime = o.realtime;
  cfg.quiet = o.quiet;
  cfg.writer = writer.get();
  cfg.replay = mode == Mode::Replay ? &replay : nullptr;
  Proxy proxy(cfg);
  std::string err;
  if (!proxy.start(o.port, &err)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  const std::string base = "http://127.0.0.1:" + std::to_string(proxy.port()) + "/v1";

  if (!o.quiet) {
    std::string what = mode == Mode::Record ? "recording to " + out_path
                                            : "replaying " + replay_path + " (" + std::to_string(replay.exchanges.size()) +
                                                  " exchanges" + (o.on_miss == "live" ? ", live on divergence" : "") + ")";
    std::fprintf(stderr, "%s %s%s\n", gold("☾ khonsu").c_str(), what.c_str(),
                 upstream ? dim("  upstream " + to_string(*upstream)).c_str() : "");
  }

  int code = 0;
  if (serve) {
    std::printf("export OPENAI_BASE_URL=%s\n", base.c_str());
    std::fflush(stdout);
    static std::atomic<bool> stop{false};
    std::signal(SIGINT, [](int) { stop = true; });
    std::signal(SIGTERM, [](int) { stop = true; });
    while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } else {
    std::vector<std::pair<std::string, std::string>> env = {
        {"OPENAI_BASE_URL", base},
        {"OPENAI_API_BASE", base},
        {"KHONSU_PROXY_URL", base},
        {"KHONSU_MODE", mode == Mode::Record ? "record" : "replay"},
    };
    if (!std::getenv("OPENAI_API_KEY")) env.emplace_back("OPENAI_API_KEY", "khonsu-no-key");  // SDKs insist on one
    code = run_child(o.argv, env, &err);
    if (code < 0) {
      std::fprintf(stderr, "khonsu: cannot run %s\n", err.c_str());
      code = 127;
    }
  }
  proxy.stop();

  const auto& st = proxy.stats();
  if (writer) {
    rec::RunEnd end;
    end.exit_code = code;
    end.duration_us = proxy.elapsed_us();
    writer->run_end(end, proxy.elapsed_us());
  }
  const auto divs = st.divergences.load();
  if (!o.quiet) {
    std::string line;
    if (mode == Mode::Record) {
      line = "recorded " + std::to_string(writer ? writer->exchanges() : 0) + " exchanges → " + out_path + " (" +
             human_bytes(log.bytes()) + (o.fsync ? ", fsync'd" : "") + ")";
    } else {
      line = "replayed " + std::to_string(st.replayed.load()) + "/" + std::to_string(replay.exchanges.size()) +
             " from recording · " + std::to_string(st.live.load()) + " live · " + std::to_string(divs) +
             " divergence" + (divs == 1 ? "" : "s");
      if (writer) line += " → " + out_path;
    }
    if (st.errors.load()) line += " · " + red(std::to_string(st.errors.load()) + " upstream errors");
    if (!serve) line += dim(" · agent exit " + std::to_string(code));
    std::fprintf(stderr, "%s %s\n", divs && o.on_miss == "fail" ? red("☾ khonsu").c_str() : green("☾ khonsu").c_str(),
                 line.c_str());
  }
  if (mode == Mode::Replay && divs > 0 && o.on_miss == "fail" && code == 0) return 3;
  return code;
}

int cmd_show(const Options& o) {
  rec::Recording r;
  if (!load_recording(o.file, r)) return 2;
  if (o.json) {
    Json j;
    j.begin_object();
    j.field("format", r.run.format).field("mode", r.run.mode).field("command", r.run.command);
    j.field("cwd", r.run.cwd).field("upstream", r.run.upstream).field("parent", r.run.parent);
    j.field("started_unix_ms", r.run.started_unix_ms).field("digest", r.digest.hex()).field("chain_ok", r.chain_ok);
    j.field("complete", r.end.has_value());
    if (r.end) j.field("exit_code", r.end->exit_code).field("duration_us", r.end->duration_us);
    j.key("exchanges").begin_array();
    for (const auto& ex : r.exchanges) {
      const auto s = rec::summarize(ex, 400);
      j.begin_object().field("seq", ex.req.seq).field("t_us", ex.req.t_us).field("method", ex.req.method);
      j.field("path", ex.req.path).field("key", ex.req.key.hex()).field("occurrence", ex.req.occurrence);
      j.field("model", s.model).field("input", s.last_user_message).field("output", s.output_text);
      j.field("tool_calls", s.tool_calls).field("prompt_tokens", s.prompt_tokens);
      j.field("completion_tokens", s.completion_tokens);
      if (ex.resp) {
        j.field("status", ex.resp->status).field("latency_us", ex.resp->latency_us).field("streamed", ex.resp->streamed);
        j.field("from_recording", ex.resp->from_recording);
      }
      j.end_object();
    }
    j.end_array().end_object();
    std::printf("%s\n", j.str().c_str());
    return 0;
  }

  std::uint64_t pt = 0, ct = 0, lat = 0;
  for (const auto& ex : r.exchanges) {
    const auto s = rec::summarize(ex);
    pt += s.prompt_tokens;
    ct += s.completion_tokens;
    if (ex.resp) lat += ex.resp->latency_us;
  }
  std::printf("%s %s\n", gold("☾").c_str(), bold(o.file).c_str());
  std::printf("  %-10s %s\n", "command", r.run.command.c_str());
  std::printf("  %-10s %s   %s\n", "mode", r.run.mode.c_str(), dim(r.run.upstream).c_str());
  if (!r.run.parent.empty()) std::printf("  %-10s %s\n", "forked", r.run.parent.substr(0, 16).c_str());
  std::printf("  %-10s %zu exchanges · %llu prompt + %llu completion tokens · %s in model calls\n", "calls",
              r.exchanges.size(), static_cast<unsigned long long>(pt), static_cast<unsigned long long>(ct),
              ms(lat).c_str());
  std::printf("  %-10s %s\n", "status",
              r.end ? ("complete · agent exit " + std::to_string(r.end->exit_code) + " · " + ms(r.end->duration_us)).c_str()
                    : gold("incomplete (the run crashed or is still going)").c_str());
  std::printf("  %-10s %s %s\n\n", "integrity", r.chain_ok ? green("hash chain ok").c_str() : red("HASH CHAIN MISMATCH").c_str(),
              dim(r.digest.hex().substr(0, 16)).c_str());
  for (const auto& ex : r.exchanges) {
    const auto s = rec::summarize(ex, 90);
    std::string status = ex.resp ? std::to_string(ex.resp->status) : red("—");
    std::printf("  %s %-4s %s  %s  %s%s\n", gold("#" + std::to_string(ex.req.seq)).c_str(), status.c_str(),
                dim(ex.resp ? ms(ex.resp->latency_us) : "").c_str(), s.model.c_str(),
                ex.resp && ex.resp->streamed ? dim("stream ").c_str() : "",
                ex.resp && ex.resp->from_recording ? dim("(replayed)").c_str() : "");
    if (!s.last_user_message.empty()) std::printf("      %s %s\n", dim("in ").c_str(), s.last_user_message.c_str());
    if (!s.output_text.empty()) std::printf("      %s %s\n", dim("out").c_str(), s.output_text.c_str());
    if (!s.tool_calls.empty()) std::printf("      %s %s\n", dim("fn ").c_str(), s.tool_calls.c_str());
  }
  return 0;
}

int cmd_verify(const Options& o) {
  std::vector<std::byte> bytes;
  std::string err;
  if (!read_file(o.file, bytes, &err)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  auto r = rec::Recording::load(bytes, &err);
  if (!r) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  const bool clean = r->recovery.reason.empty();
  std::printf("frames     %llu intact%s\n", static_cast<unsigned long long>(r->recovery.frames_recovered),
              clean ? "" : ("; " + std::to_string(r->recovery.bytes_discarded) + " bytes unreadable: " + r->recovery.reason).c_str());
  std::printf("run end    %s\n", r->end ? "present" : "missing (crashed or still running)");
  std::printf("hash chain %s\n", !r->end ? "n/a" : (r->chain_ok ? "ok" : "MISMATCH (history rewritten)"));
  std::printf("digest     %s\n", r->digest.hex().c_str());
  const bool ok = clean && r->chain_ok && r->end;
  std::printf("%s\n", ok ? green("verified").c_str() : red("NOT verified").c_str());
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::string err;
  if (!parse_args(argc, argv, o, err)) {
    if (!err.empty()) std::fprintf(stderr, "khonsu: %s\n\n", err.c_str());
    return usage(2);
  }
  if (o.cmd == "help" || o.cmd == "-h" || o.cmd == "--help") return usage(0);
  if (o.cmd == "version" || o.cmd == "--version") {
    std::printf("khonsu %s (%s TLS)\n", kVersion, tls_available() ? "with" : "without");
    return 0;
  }
  if (o.cmd == "record") {
    if (o.argv.empty()) return usage(2);
    return run_proxy(o, Mode::Record, "", o.out.empty() ? default_run_path() : o.out, false);
  }
  if (o.cmd == "replay") {
    if (o.file.empty() || o.argv.empty()) return usage(2);
    return run_proxy(o, Mode::Replay, o.file, o.out, false);
  }
  if (o.cmd == "serve") {
    if (o.record_to.empty() == o.replay_from.empty()) {
      std::fprintf(stderr, "khonsu: serve needs exactly one of --record FILE or --replay FILE\n");
      return 2;
    }
    return o.record_to.empty() ? run_proxy(o, Mode::Replay, o.replay_from, o.out, true)
                               : run_proxy(o, Mode::Record, "", o.record_to, true);
  }
  if (o.cmd == "show" && !o.file.empty()) return cmd_show(o);
  if (o.cmd == "verify" && !o.file.empty()) return cmd_verify(o);
  return usage(2);
}
