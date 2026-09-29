// khonsu — time-travel debugger for AI agents.
//
//   khonsu record [-o run.khn] [-w DIR] -- python agent.py
//   khonsu replay run.khn [--on-miss fail|live] [-o out.khn] -- python agent.py
//   khonsu fork   run.khn@N [--dir DIR] [-o fork.khn] -- python agent.py
//   khonsu checkout run.khn@N DIR · files run.khn@N · cat run.khn@N PATH
//   khonsu serve  (--record OUT | --replay IN) · show run.khn · verify run.khn
#include "file_log.hpp"
#include "net.hpp"
#include "process.hpp"
#include "proxy.hpp"
#include "workspace.hpp"

#include "khonsu/json.hpp"
#include "khonsu/recording.hpp"
#include "khonsu/rundiff.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace khonsu;
using namespace khonsu::native;

namespace {

constexpr const char* kVersion = "0.3.0";

const bool kColor = ::isatty(2);
std::string dim(const std::string& s) { return kColor ? "\033[2m" + s + "\033[0m" : s; }
std::string bold(const std::string& s) { return kColor ? "\033[1m" + s + "\033[0m" : s; }
std::string gold(const std::string& s) { return kColor ? "\033[33m" + s + "\033[0m" : s; }
std::string red(const std::string& s) { return kColor ? "\033[31m" + s + "\033[0m" : s; }
std::string green(const std::string& s) { return kColor ? "\033[32m" + s + "\033[0m" : s; }

int usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "khonsu %s: time-travel debugger for AI agents\n\n"
               "record and replay\n"
               "  khonsu record [-o FILE] [--upstream URL] [-w DIR | --no-workspace] -- CMD [ARGS...]\n"
               "      Run CMD with OPENAI_BASE_URL pointed at a recording proxy. The workspace (default:\n"
               "      the current directory if it is a git checkout) is snapshotted before every call.\n"
               "  khonsu replay FILE [--on-miss fail|live] [--upstream URL] [-o FILE] [-w DIR] [--realtime] -- CMD...\n"
               "      Run CMD against the recording: zero tokens, zero network. Model calls and\n"
               "      @khonsu.tool calls are served from the recording; the workspace is compared\n"
               "      with the recorded one before every call.\n"
               "  khonsu fork FILE@N [--dir DIR] [-o FILE] [--upstream URL] -- CMD...\n"
               "      Restore the recorded workspace into DIR, replay calls before #N, go live from #N.\n"
               "  khonsu serve (--record FILE | --replay FILE) [...]   Proxy until Ctrl-C.\n\n"
               "time travel\n"
               "  khonsu checkout FILE@REF DIR [--force]   Materialize the workspace at REF.\n"
               "  khonsu files FILE@REF                    List workspace files at REF.\n"
               "  khonsu cat FILE@REF PATH                 Print one file as it was at REF.\n"
               "      REF = start | end | N (the moment before call #N)\n\n"
               "inspect\n"
               "  khonsu show FILE [--json]    Every call, tool and workspace change.\n"
               "  khonsu verify FILE           Frame CRCs and the hash chain.\n"
               "  khonsu diff A B [--json]     Align two runs call by call; show where they diverge.\n"
               "  khonsu version\n\n"
               "upstream: --upstream, else $KHONSU_UPSTREAM, else $OPENAI_BASE_URL, else https://api.openai.com/v1\n"
               "          (local and free: --upstream http://localhost:11434/v1 for Ollama)\n",
               kVersion);
  return code;
}

struct Options {
  std::string cmd;
  std::vector<std::string> positional;
  std::string out, record_to, replay_from, upstream, workspace, dir;
  std::string on_miss = "fail";
  std::uint16_t port = 0;
  bool fsync = true, realtime = false, quiet = false, json = false, no_workspace = false, force = false;
  std::uint64_t live_from = UINT64_MAX;
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
    std::string tmp;
    if (a == "--") {
      for (++i; i < argc; ++i) o.argv.emplace_back(argv[i]);
      break;
    }
    if (a == "-o" || a == "--out") { if (!value(o.out)) return false; }
    else if (a == "--upstream") { if (!value(o.upstream)) return false; }
    else if (a == "--on-miss") { if (!value(o.on_miss)) return false; }
    else if (a == "--record") { if (!value(o.record_to)) return false; }
    else if (a == "--replay") { if (!value(o.replay_from)) return false; }
    else if (a == "-w" || a == "--workspace") { if (!value(o.workspace)) return false; }
    else if (a == "--dir") { if (!value(o.dir)) return false; }
    else if (a == "--port") { if (!value(tmp)) return false; o.port = static_cast<std::uint16_t>(std::atoi(tmp.c_str())); }
    else if (a == "--live-from") { if (!value(tmp)) return false; o.live_from = std::strtoull(tmp.c_str(), nullptr, 10); }
    else if (a == "--no-fsync") o.fsync = false;
    else if (a == "--no-workspace") o.no_workspace = true;
    else if (a == "--realtime") o.realtime = true;
    else if (a == "--force") o.force = true;
    else if (a == "-q" || a == "--quiet") o.quiet = true;
    else if (a == "--json") o.json = true;
    else if (!a.empty() && a[0] == '-') { err = "unknown option " + a; return false; }
    else o.positional.push_back(a);
  }
  if (o.on_miss != "fail" && o.on_miss != "live") {
    err = "--on-miss must be fail or live";
    return false;
  }
  return true;
}

std::string absolute(const std::string& p) {
  if (!p.empty() && p[0] == '/') return p;
  char buf[4096];
  if (::realpath(p.c_str(), buf)) return buf;
  return current_dir() + "/" + p;
}

bool is_dir(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string timestamp() {
  char buf[32];
  const std::time_t t = std::time(nullptr);
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
  return buf;
}

std::string default_run_path(const std::string& prefix = "run") {
  ::mkdir(".khonsu", 0755);
  ::mkdir(".khonsu/runs", 0755);
  return ".khonsu/runs/" + prefix + "-" + timestamp() + ".khn";
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
  if (us < 10'000) std::snprintf(buf, sizeof buf, "%.1f ms", static_cast<double>(us) / 1e3);
  else if (us < 10'000'000) std::snprintf(buf, sizeof buf, "%.0f ms", static_cast<double>(us) / 1e3);
  else std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(us) / 1e6);
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

// "run.khn@12" -> ("run.khn", "12"). No '@' -> ref "".
std::pair<std::string, std::string> split_ref(const std::string& s) {
  const auto at = s.rfind('@');
  if (at == std::string::npos || at == 0) return {s, ""};
  return {s.substr(0, at), s.substr(at + 1)};
}

const ws::Snapshot* resolve_ref(const rec::Recording& r, const std::string& ref, std::string& err) {
  const ws::Snapshot* s = nullptr;
  if (ref.empty() || ref == "end") s = r.snapshot_at(ws::When::RunEnd);
  else if (ref == "start") s = r.snapshot_at(ws::When::RunStart);
  else if (ref.find_first_not_of("0123456789") == std::string::npos) s = r.snapshot_before(std::strtoull(ref.c_str(), nullptr, 10));
  else {
    err = "REF must be start, end or a call number";
    return nullptr;
  }
  if (!s) err = r.snapshots.empty() ? "this recording has no workspace snapshots (recorded with --no-workspace?)"
                                    : "no workspace snapshot at @" + ref;
  return s;
}

// ------------------------------------------------------------------------------------------
// record / replay / fork / serve
// ------------------------------------------------------------------------------------------

struct RunPlan {
  Mode mode = Mode::Record;
  std::string replay_path, out_path, workspace, child_cwd;
  bool serve = false, fork = false;
};

int run_proxy(const Options& o, const RunPlan& plan) {
  rec::Recording replay;
  if (plan.mode == Mode::Replay && !load_recording(plan.replay_path, replay)) return 2;

  const bool need_upstream = plan.mode == Mode::Record || o.on_miss == "live" || plan.fork;
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
  if (!plan.out_path.empty()) {
    std::string err;
    RecoveryReport rep;
    if (!log.open(plan.out_path, o.fsync ? SyncPolicy::Always : SyncPolicy::None, &err, &rep)) {
      std::fprintf(stderr, "khonsu: %s\n", err.c_str());
      return 2;
    }
    if (log.frames() > 0) {
      std::fprintf(stderr, "khonsu: %s already contains a recording; choose a new file\n", plan.out_path.c_str());
      return 2;
    }
    writer = std::make_unique<RecordingWriter>(log);
    rec::RunInfo info;
    info.tool_version = kVersion;
    info.mode = plan.mode == Mode::Record ? "record" : plan.fork ? "fork" : (o.on_miss == "live" ? "replay-then-live" : "replay");
    info.command = plan.serve ? "(serve)" : shell_join(o.argv);
    info.cwd = plan.child_cwd.empty() ? current_dir() : plan.child_cwd;
    info.upstream = upstream ? to_string(*upstream) : "";
    info.parent = plan.mode == Mode::Replay ? replay.digest.hex() : "";
    info.workspace = plan.workspace;
    info.started_unix_ms = now_unix_ms();
    writer->run_start(info);
  }

  std::unique_ptr<Snapshotter> snapper;
  std::unique_ptr<WorkspaceTracker> tracker;
  if (!plan.workspace.empty()) {
    snapper = std::make_unique<Snapshotter>(plan.workspace);
    tracker = std::make_unique<WorkspaceTracker>(*snapper, writer.get(),
                                                 plan.mode == Mode::Replay && !replay.snapshots.empty() ? &replay : nullptr,
                                                 o.quiet);
    tracker->at_start();
  }

  ProxyConfig cfg;
  cfg.mode = plan.mode;
  cfg.on_miss = o.on_miss == "live" || plan.fork ? OnMiss::Live : OnMiss::Fail;
  cfg.upstream = upstream;
  cfg.realtime = o.realtime;
  cfg.quiet = o.quiet;
  cfg.writer = writer.get();
  cfg.replay = plan.mode == Mode::Replay ? &replay : nullptr;
  cfg.live_from = o.live_from;
  cfg.workspace = tracker.get();
  Proxy proxy(cfg);
  std::string err;
  if (!proxy.start(o.port, &err)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  const std::string origin = "http://127.0.0.1:" + std::to_string(proxy.port());
  const std::string base = origin + "/v1";

  if (!o.quiet) {
    std::string what;
    if (plan.mode == Mode::Record) what = "recording to " + plan.out_path;
    else if (plan.fork) what = "forking " + plan.replay_path + " at call #" + std::to_string(o.live_from) + " in " + plan.child_cwd;
    else what = "replaying " + plan.replay_path + " (" + std::to_string(replay.exchanges.size()) + " calls" +
                (o.on_miss == "live" ? ", live on divergence" : "") + ")";
    std::string extra;
    if (upstream) extra += "  upstream " + to_string(*upstream);
    if (!plan.workspace.empty()) extra += "  workspace " + plan.workspace;
    std::fprintf(stderr, "%s %s%s\n", gold("☾ khonsu").c_str(), what.c_str(), dim(extra).c_str());
  }

  int code = 0;
  if (plan.serve) {
    std::printf("export OPENAI_BASE_URL=%s\nexport KHONSU_TOOL_URL=%s/khonsu/v1/tool\n", base.c_str(), origin.c_str());
    std::fflush(stdout);
    static std::atomic<bool> stop{false};
    std::signal(SIGINT, [](int) { stop = true; });
    std::signal(SIGTERM, [](int) { stop = true; });
    while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } else {
    std::vector<std::pair<std::string, std::string>> env = {
        {"OPENAI_BASE_URL", base},
        {"OPENAI_API_BASE", base},
        {"KHONSU_PROXY_URL", origin},
        {"KHONSU_TOOL_URL", origin + "/khonsu/v1/tool"},
        {"KHONSU_MODE", plan.mode == Mode::Record ? "record" : "replay"},
    };
    if (!std::getenv("OPENAI_API_KEY")) env.emplace_back("OPENAI_API_KEY", "khonsu-no-key");  // SDKs insist on one
    code = run_child(o.argv, env, plan.child_cwd, &err);
    if (code < 0) {
      std::fprintf(stderr, "khonsu: cannot run %s\n", err.c_str());
      code = 127;
    }
  }
  proxy.stop();
  if (tracker) tracker->at_end(!plan.fork);

  const auto& st = proxy.stats();
  if (writer) {
    rec::RunEnd end;
    end.exit_code = code;
    end.duration_us = proxy.elapsed_us();
    writer->run_end(end, proxy.elapsed_us());
  }
  const auto divs = st.divergences.load();
  const auto ws_divs = tracker ? tracker->divergences() : 0;
  if (!o.quiet) {
    std::string line;
    const auto tools = st.tools_replayed.load() + st.tools_executed.load();
    if (plan.mode == Mode::Record) {
      line = "recorded " + std::to_string(writer ? writer->exchanges() : 0) + " calls";
      if (tools) line += " (" + std::to_string(st.tools_executed.load()) + " tools)";
      if (tracker) line += " · " + std::to_string(tracker->snapshots()) + " workspace snapshots";
      line += " → " + plan.out_path + " (" + human_bytes(log.bytes()) + (o.fsync ? ", fsync'd" : "") + ")";
    } else {
      line = "replayed " + std::to_string(st.replayed.load() + st.tools_replayed.load()) + "/" +
             std::to_string(replay.exchanges.size()) + " from recording · " +
             std::to_string(st.live.load() + st.tools_executed.load()) + " live · " + std::to_string(divs) +
             " divergence" + (divs == 1 ? "" : "s");
      if (tracker)
        line += " · workspace " +
                (ws_divs ? gold(std::to_string(ws_divs) + (ws_divs == 1 ? " mismatch" : " mismatches")) : std::string("identical"));
      if (writer) line += " → " + plan.out_path;
    }
    if (st.errors.load()) line += " · " + red(std::to_string(st.errors.load()) + " upstream errors");
    if (!plan.serve) line += dim(" · agent exit " + std::to_string(code));
    const bool bad = divs && o.on_miss == "fail" && !plan.fork;
    std::fprintf(stderr, "%s %s\n", bad ? red("☾ khonsu").c_str() : green("☾ khonsu").c_str(), line.c_str());
  }
  if (plan.mode == Mode::Replay && divs > 0 && o.on_miss == "fail" && !plan.fork && code == 0) return 3;
  return code;
}

std::string default_workspace(const Options& o) {
  if (o.no_workspace) return "";
  if (!o.workspace.empty()) return absolute(o.workspace);
  return is_dir(".git") ? current_dir() : "";  // a project checkout: capture it by default
}

int cmd_fork(const Options& o) {
  if (o.positional.empty() || o.argv.empty()) return usage(2);
  const auto [file, ref] = split_ref(o.positional[0]);
  if (ref.empty() || ref.find_first_not_of("0123456789") != std::string::npos) {
    std::fprintf(stderr, "khonsu: fork needs FILE@N (the call to go live from)\n");
    return 2;
  }
  rec::Recording r;
  if (!load_recording(file, r)) return 2;
  const ws::Snapshot* start = r.snapshot_at(ws::When::RunStart);
  const std::string stem = file.substr(file.find_last_of('/') + 1, file.rfind(".khn") - file.find_last_of('/') - 1);
  std::string dir = o.dir.empty() ? ".khonsu/forks/" + stem + "-at-" + ref + "-" + timestamp() : o.dir;
  if (start) {
    ::mkdir(".khonsu", 0755);
    ::mkdir(".khonsu/forks", 0755);
    std::vector<std::string> skipped;
    std::string err;
    if (!checkout(r.objects, start->root, dir, o.force, &skipped, &err)) {
      std::fprintf(stderr, "khonsu: %s\n", err.c_str());
      return 2;
    }
    if (!skipped.empty()) std::fprintf(stderr, "khonsu: %zu large files were not stored and are missing in the fork\n", skipped.size());
  } else {
    std::fprintf(stderr, "%s\n", gold("khonsu: no workspace in this recording; forking the conversation only").c_str());
    if (o.dir.empty()) dir = current_dir();
  }
  Options f = o;
  f.live_from = std::strtoull(ref.c_str(), nullptr, 10);
  RunPlan plan;
  plan.mode = Mode::Replay;
  plan.fork = true;
  plan.replay_path = absolute(file);
  plan.child_cwd = absolute(dir);
  plan.workspace = start ? plan.child_cwd : "";
  plan.out_path = o.out.empty() ? absolute(default_run_path(stem + "-fork-at-" + ref)) : absolute(o.out);
  return run_proxy(f, plan);
}

// ------------------------------------------------------------------------------------------
// time travel: checkout / files / cat
// ------------------------------------------------------------------------------------------

int cmd_checkout(const Options& o) {
  if (o.positional.size() != 2) return usage(2);
  const auto [file, ref] = split_ref(o.positional[0]);
  rec::Recording r;
  if (!load_recording(file, r)) return 2;
  std::string err;
  const ws::Snapshot* s = resolve_ref(r, ref, err);
  if (!s) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  std::vector<std::string> skipped;
  if (!checkout(r.objects, s->root, o.positional[1], o.force, &skipped, &err)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  std::fprintf(stderr, "%s %s@%s → %s (%llu files, %s)%s\n", green("☾ khonsu").c_str(), file.c_str(),
               ref.empty() ? "end" : ref.c_str(), o.positional[1].c_str(), static_cast<unsigned long long>(s->files),
               human_bytes(s->bytes).c_str(),
               skipped.empty() ? "" : (" · " + std::to_string(skipped.size()) + " large files not stored").c_str());
  return 0;
}

int cmd_files(const Options& o, bool cat) {
  if (o.positional.size() != (cat ? 2u : 1u)) return usage(2);
  const auto [file, ref] = split_ref(o.positional[0]);
  rec::Recording r;
  if (!load_recording(file, r)) return 2;
  std::string err;
  const ws::Snapshot* s = resolve_ref(r, ref, err);
  if (!s) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  for (const auto& f : ws::list_files(r.objects, s->root)) {
    if (!cat) {
      std::printf("%s  %8s  %s%s\n", f.entry.id.hex().substr(0, 10).c_str(), human_bytes(f.entry.size).c_str(),
                  f.path.c_str(), f.entry.type == ws::EntryType::Omitted ? "  (not stored)" : "");
      continue;
    }
    if (f.path != o.positional[1]) continue;
    const auto* b = r.objects.get(f.entry.id);
    if (!b) {
      std::fprintf(stderr, "khonsu: %s was too large to store\n", f.path.c_str());
      return 1;
    }
    std::fwrite(b->data(), 1, b->size(), stdout);
    return 0;
  }
  if (cat) {
    std::fprintf(stderr, "khonsu: %s did not exist at @%s\n", o.positional[1].c_str(), ref.empty() ? "end" : ref.c_str());
    return 1;
  }
  return 0;
}

// ------------------------------------------------------------------------------------------
// show / verify
// ------------------------------------------------------------------------------------------

// One line per field in the human view: newlines become a visible ↵.
std::string oneline(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '\n') out += " ↵ ";
    else if (c != '\r') out += c;
  }
  return out;
}

int cmd_show(const Options& o) {
  if (o.positional.size() != 1) return usage(2);
  const std::string& path = o.positional[0];
  rec::Recording r;
  if (!load_recording(path, r)) return 2;

  // Workspace change caused by each call: diff of the snapshots before and after it.
  auto ws_change = [&](std::size_t i) -> std::vector<ws::FileChange> {
    const auto& ex = r.exchanges[i];
    const ws::Snapshot* before = r.snapshot_before(ex.req.seq);
    const ws::Snapshot* after = i + 1 < r.exchanges.size() ? r.snapshot_before(r.exchanges[i + 1].req.seq)
                                                            : r.snapshot_at(ws::When::RunEnd);
    if (!before || !after) return {};
    return ws::diff(r.objects, before->root, r.objects, after->root);
  };

  if (o.json) {
    Json j;
    j.begin_object();
    j.field("format", r.run.format).field("mode", r.run.mode).field("command", r.run.command);
    j.field("cwd", r.run.cwd).field("upstream", r.run.upstream).field("parent", r.run.parent);
    j.field("workspace", r.run.workspace).field("snapshots", r.snapshots.size()).field("objects", r.objects.size());
    j.field("object_bytes", r.objects.bytes());
    j.field("started_unix_ms", r.run.started_unix_ms).field("digest", r.digest.hex()).field("chain_ok", r.chain_ok);
    j.field("complete", r.end.has_value());
    if (r.end) j.field("exit_code", r.end->exit_code).field("duration_us", r.end->duration_us);
    j.key("snapshot_list").begin_array();
    for (const auto& s : r.snapshots) {
      j.begin_object().field("when", static_cast<int>(s.when)).field("seq", s.seq).field("root", s.root.hex());
      j.field("files", s.files).field("bytes", s.bytes).field("new_objects", s.new_objects);
      j.field("new_bytes", s.new_bytes).field("took_us", s.took_us).end_object();
    }
    j.end_array();
    j.key("exchanges").begin_array();
    for (std::size_t i = 0; i < r.exchanges.size(); ++i) {
      const auto& ex = r.exchanges[i];
      const auto s = rec::summarize(ex, 400);
      j.begin_object().field("seq", ex.req.seq).field("t_us", ex.req.t_us).field("method", ex.req.method);
      j.field("path", ex.req.path).field("key", ex.req.key.hex()).field("occurrence", ex.req.occurrence);
      j.field("tool", ex.req.path == rec::kToolPath);
      j.field("model", s.model).field("input", s.last_user_message).field("output", s.output_text);
      j.field("tool_calls", s.tool_calls).field("prompt_tokens", s.prompt_tokens);
      j.field("completion_tokens", s.completion_tokens);
      if (ex.resp) {
        j.field("status", ex.resp->status).field("latency_us", ex.resp->latency_us).field("streamed", ex.resp->streamed);
        j.field("from_recording", ex.resp->from_recording);
      }
      j.key("workspace_changes").begin_array();
      for (const auto& c : ws_change(i)) j.value(std::string(1, c.kind) + " " + c.path);
      j.end_array();
      j.end_object();
    }
    j.end_array().end_object();
    std::printf("%s\n", j.str().c_str());
    return 0;
  }

  std::uint64_t pt = 0, ct = 0, lat = 0, tools = 0;
  for (const auto& ex : r.exchanges) {
    const auto s = rec::summarize(ex);
    pt += s.prompt_tokens;
    ct += s.completion_tokens;
    if (ex.req.path == rec::kToolPath) ++tools;
    else if (ex.resp) lat += ex.resp->latency_us;
  }
  std::printf("%s %s\n", gold("☾").c_str(), bold(path).c_str());
  std::printf("  %-10s %s\n", "command", r.run.command.c_str());
  std::printf("  %-10s %s   %s\n", "mode", r.run.mode.c_str(), dim(r.run.upstream).c_str());
  if (!r.run.parent.empty()) std::printf("  %-10s %s\n", "parent", r.run.parent.substr(0, 16).c_str());
  std::printf("  %-10s %zu model + %llu tool · %llu prompt + %llu completion tokens · %s in model calls\n", "calls",
              r.exchanges.size() - tools, static_cast<unsigned long long>(tools), static_cast<unsigned long long>(pt),
              static_cast<unsigned long long>(ct), ms(lat).c_str());
  if (!r.snapshots.empty())
    std::printf("  %-10s %s · %zu snapshots · %zu objects (%s, deduplicated)\n", "workspace", r.run.workspace.c_str(),
                r.snapshots.size(), r.objects.size(), human_bytes(r.objects.bytes()).c_str());
  std::printf("  %-10s %s\n", "status",
              r.end ? ("complete · agent exit " + std::to_string(r.end->exit_code) + " · " + ms(r.end->duration_us)).c_str()
                    : gold("incomplete (the run crashed or is still going)").c_str());
  std::printf("  %-10s %s %s\n\n", "integrity", r.chain_ok ? green("hash chain ok").c_str() : red("HASH CHAIN MISMATCH").c_str(),
              dim(r.digest.hex().substr(0, 16)).c_str());
  for (std::size_t i = 0; i < r.exchanges.size(); ++i) {
    const auto& ex = r.exchanges[i];
    const auto s = rec::summarize(ex, 90);
    const bool tool = ex.req.path == rec::kToolPath;
    std::string status = ex.resp ? std::to_string(ex.resp->status) : red("—");
    std::printf("  %s %-4s %s  %s  %s%s\n", gold("#" + std::to_string(ex.req.seq)).c_str(), tool ? "tool" : status.c_str(),
                dim(ex.resp ? ms(ex.resp->latency_us) : "").c_str(), tool ? s.tool_calls.c_str() : s.model.c_str(),
                ex.resp && ex.resp->streamed ? dim("stream ").c_str() : "",
                ex.resp && ex.resp->from_recording ? dim("(replayed)").c_str() : "");
    if (!s.last_user_message.empty())
      std::printf("      %s %s\n", dim(tool ? "arg" : "in ").c_str(), oneline(s.last_user_message).c_str());
    if (!s.output_text.empty()) std::printf("      %s %s\n", dim(tool ? "ret" : "out").c_str(), oneline(s.output_text).c_str());
    if (!tool && !s.tool_calls.empty()) std::printf("      %s %s\n", dim("fn ").c_str(), s.tool_calls.c_str());
    if (const auto ch = ws_change(i); !ch.empty()) std::printf("      %s %s\n", dim("ws ").c_str(), describe_changes(ch).c_str());
  }
  return 0;
}

int cmd_diff(const Options& o) {
  if (o.positional.size() != 2) return usage(2);
  rec::Recording a, b;
  if (!load_recording(o.positional[0], a) || !load_recording(o.positional[1], b)) return 2;
  const rec::RunDiff d = rec::diff_runs(a, b);
  const bool identical = d.first_divergence == SIZE_MAX && d.workspace_end.empty();
  auto label = [](const rec::Exchange* e) -> std::string {
    if (!e) return "";
    const auto s = rec::summarize(*e, 60);
    return e->req.path == rec::kToolPath ? "tool " + s.tool_calls : s.model;
  };
  auto seq = [](const rec::Exchange* e) { return e ? "#" + std::to_string(e->req.seq) : std::string("—"); };

  if (o.json) {
    Json j;
    j.begin_object().field("identical", identical).field("same", d.same).field("a_calls", a.exchanges.size());
    j.field("b_calls", b.exchanges.size()).field("a_tokens", d.a_tokens).field("b_tokens", d.b_tokens);
    j.field("a_model_us", d.a_model_us).field("b_model_us", d.b_model_us);
    if (d.first_divergence == SIZE_MAX) j.key("first_divergence").null();
    else j.field("first_divergence", d.first_divergence);
    j.key("calls").begin_array();
    for (const auto& c : d.calls) {
      j.begin_object().field("kind", rec::kind_name(c.kind)).field("label", label(c.a ? c.a : c.b));
      if (c.a) j.field("a_seq", c.a->req.seq);
      if (c.b) j.field("b_seq", c.b->req.seq);
      if (!c.field.empty()) j.field("field", c.field);
      if (!c.a_value.empty() || !c.b_value.empty()) j.field("a", c.a_value).field("b", c.b_value);
      j.key("workspace_before").begin_array();
      for (const auto& f : c.workspace_before) j.value(std::string(1, f.kind) + " " + f.path);
      j.end_array().end_object();
    }
    j.end_array().key("workspace_end").begin_array();
    for (const auto& f : d.workspace_end) j.value(std::string(1, f.kind) + " " + f.path);
    j.end_array().end_object();
    std::printf("%s\n", j.str().c_str());
    return identical ? 0 : 1;
  }

  auto delta = [](std::uint64_t x, std::uint64_t y) {
    const long long dd = static_cast<long long>(y) - static_cast<long long>(x);
    return dd == 0 ? std::string() : std::string(" (") + (dd > 0 ? "+" : "") + std::to_string(dd) + ")";
  };
  std::printf("%s %s %s %s\n", gold("☾").c_str(), bold(o.positional[0]).c_str(), dim("vs").c_str(), bold(o.positional[1]).c_str());
  std::printf("  %-10s A %zu · B %zu · %zu identical\n", "calls", a.exchanges.size(), b.exchanges.size(), d.same);
  std::printf("  %-10s A %llu · B %llu%s\n", "tokens", static_cast<unsigned long long>(d.a_tokens),
              static_cast<unsigned long long>(d.b_tokens), delta(d.a_tokens, d.b_tokens).c_str());
  std::printf("  %-10s A %s · B %s\n\n", "model time", ms(d.a_model_us).c_str(), ms(d.b_model_us).c_str());
  if (identical) {
    std::printf("  %s\n", green("identical: same calls, same outputs, same workspace").c_str());
    return 0;
  }
  if (d.first_divergence != SIZE_MAX) {
    const auto& c = d.calls[d.first_divergence];
    std::string what = c.kind == rec::CallDiffKind::SameRequest ? "same request, different output"
                       : c.kind == rec::CallDiffKind::Changed   ? "the request differs at `" + c.field + "`"
                       : c.kind == rec::CallDiffKind::OnlyA     ? "only run A made this call"
                                                                : "only run B made this call";
    std::printf("  %s A%s / B%s (%s): %s\n", gold("first divergence").c_str(), seq(c.a).c_str(), seq(c.b).c_str(),
                label(c.a ? c.a : c.b).c_str(), what.c_str());
    if (!c.a_value.empty() || !c.b_value.empty()) {
      std::printf("    A  %s\n", oneline(c.a_value).c_str());
      std::printf("    B  %s\n", oneline(c.b_value).c_str());
    }
    if (!c.workspace_before.empty())
      std::printf("    %s %s\n", dim("files already differed:").c_str(), describe_changes(c.workspace_before).c_str());
    std::printf("\n");
  }
  for (const auto& c : d.calls) {
    const char* mark = c.kind == rec::CallDiffKind::Same          ? "="
                       : c.kind == rec::CallDiffKind::SameRequest ? "~"
                       : c.kind == rec::CallDiffKind::Changed     ? "≠"
                       : c.kind == rec::CallDiffKind::OnlyA       ? "-"
                                                                  : "+";
    const std::string m = c.kind == rec::CallDiffKind::Same ? dim(mark) : gold(mark);
    std::printf("  %s A%-4s B%-4s %s", m.c_str(), seq(c.a).c_str(), seq(c.b).c_str(), label(c.a ? c.a : c.b).c_str());
    if (c.kind == rec::CallDiffKind::Changed && !c.field.empty()) std::printf("  %s", dim("at " + c.field).c_str());
    std::printf("\n");
  }
  if (!d.workspace_end.empty())
    std::printf("\n  %s %s\n", gold("workspace at end:").c_str(), describe_changes(d.workspace_end, 12).c_str());
  return 1;
}

int cmd_verify(const Options& o) {
  if (o.positional.size() != 1) return usage(2);
  std::vector<std::byte> bytes;
  std::string err;
  if (!read_file(o.positional[0], bytes, &err)) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  auto r = rec::Recording::load(bytes, &err);
  if (!r) {
    std::fprintf(stderr, "khonsu: %s\n", err.c_str());
    return 2;
  }
  // Every snapshot must be fully restorable from the objects in the file.
  std::size_t missing = 0;
  for (const auto& s : r->snapshots)
    for (const auto& f : ws::list_files(r->objects, s.root))
      if (f.entry.type == ws::EntryType::File && !r->objects.contains(f.entry.id)) ++missing;
  const bool clean = r->recovery.reason.empty();
  std::printf("frames     %llu intact%s\n", static_cast<unsigned long long>(r->recovery.frames_recovered),
              clean ? "" : ("; " + std::to_string(r->recovery.bytes_discarded) + " bytes unreadable: " + r->recovery.reason).c_str());
  std::printf("run end    %s\n", r->end ? "present" : "missing (crashed or still running)");
  std::printf("hash chain %s\n", !r->end ? "n/a" : (r->chain_ok ? "ok" : "MISMATCH (history rewritten)"));
  std::printf("workspace  %zu snapshots, %zu objects%s\n", r->snapshots.size(), r->objects.size(),
              missing ? (", " + std::to_string(missing) + " MISSING BLOBS").c_str() : ", all restorable");
  std::printf("digest     %s\n", r->digest.hex().c_str());
  const bool ok = clean && r->chain_ok && r->end && missing == 0;
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
    if (o.argv.empty() || !o.positional.empty()) return usage(2);
    RunPlan plan;
    plan.mode = Mode::Record;
    plan.out_path = o.out.empty() ? default_run_path() : o.out;
    plan.workspace = default_workspace(o);
    return run_proxy(o, plan);
  }
  if (o.cmd == "replay") {
    if (o.positional.size() != 1 || o.argv.empty()) return usage(2);
    rec::Recording peek;
    if (!load_recording(o.positional[0], peek, false)) return 2;
    RunPlan plan;
    plan.mode = Mode::Replay;
    plan.replay_path = o.positional[0];
    plan.out_path = o.out;
    // Compare the workspace if asked, or if we're replaying in the directory that was recorded.
    if (!o.no_workspace && !peek.snapshots.empty()) {
      if (!o.workspace.empty()) plan.workspace = absolute(o.workspace);
      else if (!peek.run.workspace.empty() && absolute(".") == peek.run.workspace) plan.workspace = peek.run.workspace;
    }
    return run_proxy(o, plan);
  }
  if (o.cmd == "fork") return cmd_fork(o);
  if (o.cmd == "serve") {
    if (o.record_to.empty() == o.replay_from.empty()) {
      std::fprintf(stderr, "khonsu: serve needs exactly one of --record FILE or --replay FILE\n");
      return 2;
    }
    RunPlan plan;
    plan.serve = true;
    plan.mode = o.record_to.empty() ? Mode::Replay : Mode::Record;
    plan.replay_path = o.replay_from;
    plan.out_path = o.record_to.empty() ? o.out : o.record_to;
    plan.workspace = o.workspace.empty() ? "" : absolute(o.workspace);
    return run_proxy(o, plan);
  }
  if (o.cmd == "checkout") return cmd_checkout(o);
  if (o.cmd == "files") return cmd_files(o, false);
  if (o.cmd == "cat") return cmd_files(o, true);
  if (o.cmd == "show") return cmd_show(o);
  if (o.cmd == "verify") return cmd_verify(o);
  if (o.cmd == "diff") return cmd_diff(o);
  return usage(2);
}
