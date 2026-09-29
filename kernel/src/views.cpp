#include "khonsu/views.hpp"

#include "khonsu/json.hpp"
#include "khonsu/pack.hpp"

#include <algorithm>

namespace khonsu::views {

namespace {

std::vector<ws::FileChange> call_changes(const rec::Recording& r, std::size_t i) {
  const auto& ex = r.exchanges[i];
  const ws::Snapshot* before = r.snapshot_before(ex.req.seq);
  const ws::Snapshot* after = i + 1 < r.exchanges.size() ? r.snapshot_before(r.exchanges[i + 1].req.seq)
                                                          : r.snapshot_at(ws::When::RunEnd);
  if (!before || !after) return {};
  return ws::diff(r.objects, before->root, r.objects, after->root);
}

bool is_text(const std::vector<std::byte>& b) {
  if (b.size() > (512u << 10)) return false;
  return std::none_of(b.begin(), b.end(), [](std::byte c) { return c == std::byte{0}; });
}

std::string text_of(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

Digest128 hex(const std::string& h) { return pack::parse_hex(h); }

void pretty_or_raw(Json& j, std::string_view key, const std::string& body) {
  if (auto v = jsonv::parse(body)) j.key(key).raw(jsonv::canonical(*v));
  else j.field(key, body);
}

}  // namespace

std::string recording(const rec::Recording& r) {
  Json j;
  j.begin_object();
  j.field("format", r.run.format).field("mode", r.run.mode).field("command", r.run.command);
  j.field("cwd", r.run.cwd).field("upstream", r.run.upstream).field("parent", r.run.parent);
  j.field("workspace", r.run.workspace).field("tool_version", r.run.tool_version);
  j.field("snapshots", r.snapshots.size()).field("objects", r.objects.size()).field("object_bytes", r.objects.bytes());
  j.field("started_unix_ms", r.run.started_unix_ms).field("digest", r.digest.hex()).field("chain_ok", r.chain_ok);
  j.field("complete", r.end.has_value()).field("recovery", r.recovery.reason);
  j.field("frames", r.recovery.frames_recovered);
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
    if (const auto* sb = r.snapshot_before(ex.req.seq)) j.field("snapshot", sb->root.hex());
    if (ex.resp) {
      j.field("status", ex.resp->status).field("latency_us", ex.resp->latency_us).field("streamed", ex.resp->streamed);
      j.field("from_recording", ex.resp->from_recording);
    }
    j.key("workspace_changes").begin_array();
    for (const auto& c : call_changes(r, i)) j.value(std::string(1, c.kind) + " " + c.path);
    j.end_array();
    j.end_object();
  }
  j.end_array().end_object();
  return j.take();
}

std::string call(const rec::Recording& r, std::size_t index) {
  Json j;
  j.begin_object();
  if (index >= r.exchanges.size()) {
    j.field("error", "no such call").end_object();
    return j.take();
  }
  const auto& ex = r.exchanges[index];
  j.field("seq", ex.req.seq).field("method", ex.req.method).field("path", ex.req.path);
  j.field("occurrence", ex.req.occurrence).field("key", ex.req.key.hex());
  j.key("headers").raw(ex.req.headers_json.empty() ? "{}" : ex.req.headers_json);
  pretty_or_raw(j, "request", ex.req.body);
  if (ex.resp) {
    j.key("response").begin_object().field("status", ex.resp->status).field("content_type", ex.resp->content_type);
    j.field("latency_us", ex.resp->latency_us).field("streamed", ex.resp->streamed);
    j.field("from_recording", ex.resp->from_recording).field("chunks", ex.resp->chunks.size());
    if (ex.resp->streamed) j.field("body", ex.resp->body);
    else pretty_or_raw(j, "body", ex.resp->body);
    j.key("timing").begin_array();
    for (const auto& c : ex.resp->chunks) j.begin_array().value(c.end).value(c.t_us).end_array();
    j.end_array().end_object();
  }
  j.end_object();
  return j.take();
}

std::string files(const rec::Recording& r, const std::string& root_hex) {
  Json j;
  j.begin_array();
  for (const auto& f : ws::list_files(r.objects, hex(root_hex))) {
    j.begin_object().field("path", f.path).field("size", f.entry.size).field("id", f.entry.id.hex());
    j.field("stored", f.entry.type != ws::EntryType::Omitted).field("symlink", f.entry.type == ws::EntryType::Symlink);
    j.end_object();
  }
  j.end_array();
  return j.take();
}

std::string file(const rec::Recording& r, const std::string& root_hex, const std::string& path) {
  Json j;
  j.begin_object().field("path", path);
  for (const auto& f : ws::list_files(r.objects, hex(root_hex))) {
    if (f.path != path) continue;
    const auto* b = r.objects.get(f.entry.id);
    j.field("size", f.entry.size).field("stored", b != nullptr);
    if (b && is_text(*b)) j.field("binary", false).field("text", text_of(*b));
    else j.field("binary", b != nullptr).key("text").null();
    j.end_object();
    return j.take();
  }
  j.field("missing", true).end_object();
  return j.take();
}

std::string changes(const rec::Recording& r, const std::string& from_hex, const std::string& to_hex) {
  Json j;
  j.begin_array();
  for (const auto& c : ws::diff(r.objects, hex(from_hex), r.objects, hex(to_hex))) {
    j.begin_object().field("kind", std::string(1, c.kind)).field("path", c.path);
    const auto* before = c.before.empty() ? nullptr : r.objects.get(c.before);
    const auto* after = c.after.empty() ? nullptr : r.objects.get(c.after);
    if ((!before || is_text(*before)) && (!after || is_text(*after)) && (before || after)) {
      j.field("before", before ? text_of(*before) : "").field("after", after ? text_of(*after) : "");
    }
    j.end_object();
  }
  j.end_array();
  return j.take();
}

std::string diff(const rec::RunDiff& d, const rec::Recording& a, const rec::Recording& b) {
  const bool identical = d.first_divergence == SIZE_MAX && d.workspace_end.empty();
  auto label = [](const rec::Exchange* e) -> std::string {
    if (!e) return "";
    const auto s = rec::summarize(*e, 60);
    return e->req.path == rec::kToolPath ? "tool " + s.tool_calls : s.model;
  };
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
  return j.take();
}

}  // namespace khonsu::views
