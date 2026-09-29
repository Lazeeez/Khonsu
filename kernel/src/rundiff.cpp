#include "khonsu/rundiff.hpp"

#include <algorithm>

namespace khonsu::rec {

namespace {

// Identity of a call for alignment: its match key and occurrence (so the 2nd
// identical call aligns with the 2nd, not the 1st).
struct CallId {
  Digest128 key;
  std::uint32_t occurrence;
  friend bool operator==(const CallId&, const CallId&) = default;
};

// What a call produced, in comparable form: text plus tool calls *with arguments*
// (ids and timestamps in the raw body differ on every call and are ignored).
std::string output_of(const Exchange& e) {
  const auto s = summarize(e, 100'000);
  std::string out = s.output_text;
  if (e.req.path == kToolPath || !e.resp || e.resp->streamed) {
    if (!s.tool_calls.empty() && e.req.path != kToolPath) out += " → " + s.tool_calls;
    return out;
  }
  if (auto body = jsonv::parse(e.resp->body))
    if (const auto* ch = body->get("choices"); ch && ch->at(0))
      if (const auto* m = ch->at(0)->get("message"))
        if (const auto* calls = m->get("tool_calls"); calls && calls->kind() == jsonv::Kind::Array)
          for (const auto& c : calls->as_array())
            if (const auto* fn = c.get("function"))
              out += " → " + std::string(fn->str("name")) + "(" + std::string(fn->str("arguments")) + ")";
  return out;
}

std::string clip(const std::string& s, std::size_t n = 240) { return s.size() <= n ? s : s.substr(0, n) + "…"; }

const jsonv::Value* resolve(const jsonv::Value& root, const std::string& path) {
  const jsonv::Value* cur = &root;
  std::size_t i = 0;
  while (cur && i < path.size()) {
    if (path[i] == '.') {
      ++i;
    } else if (path[i] == '[') {
      const auto close = path.find(']', i);
      if (close == std::string::npos) return nullptr;
      cur = cur->at(static_cast<std::size_t>(std::stoull(path.substr(i + 1, close - i - 1))));
      i = close + 1;
    } else {
      auto end = path.find_first_of(".[", i);
      if (end == std::string::npos) end = path.size();
      cur = cur->get(path.substr(i, end - i));
      i = end;
    }
  }
  return cur;
}

std::string render(const jsonv::Value* v) {
  if (!v) return "(absent)";
  return clip(v->kind() == jsonv::Kind::String ? v->as_string() : jsonv::canonical(*v));
}

}  // namespace

const char* kind_name(CallDiffKind k) {
  switch (k) {
    case CallDiffKind::Same: return "same";
    case CallDiffKind::SameRequest: return "same-request";
    case CallDiffKind::Changed: return "changed";
    case CallDiffKind::OnlyA: return "only-a";
    case CallDiffKind::OnlyB: return "only-b";
  }
  return "?";
}

RunDiff diff_runs(const Recording& a, const Recording& b) {
  RunDiff d;
  const auto& xs = a.exchanges;
  const auto& ys = b.exchanges;
  auto id = [](const Exchange& e) { return CallId{e.req.key, e.req.occurrence}; };

  // LCS over call identities (runs are at most thousands of calls: O(n·m) is fine).
  const std::size_t n = xs.size(), m = ys.size();
  std::vector<std::vector<std::uint32_t>> dp(n + 1, std::vector<std::uint32_t>(m + 1, 0));
  for (std::size_t i = n; i-- > 0;)
    for (std::size_t j = m; j-- > 0;)
      dp[i][j] = id(xs[i]) == id(ys[j]) ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);

  // Walk the alignment; unmatched stretches between anchors are paired positionally.
  std::vector<std::size_t> gap_a, gap_b;
  auto flush = [&] {
    const std::size_t k = std::min(gap_a.size(), gap_b.size());
    for (std::size_t t = 0; t < k; ++t) {
      CallDiff c;
      c.kind = CallDiffKind::Changed;
      c.a = &xs[gap_a[t]];
      c.b = &ys[gap_b[t]];
      if (c.a->req.path != c.b->req.path) {
        c.field = "(different endpoint)";
        c.a_value = c.a->req.path;
        c.b_value = c.b->req.path;
      } else if (auto ja = jsonv::parse(c.a->req.body), jb = jsonv::parse(c.b->req.body); ja && jb) {
        c.field = jsonv::first_difference(*ja, *jb);
        c.a_value = render(resolve(*ja, c.field));
        c.b_value = render(resolve(*jb, c.field));
      }
      d.calls.push_back(std::move(c));
    }
    for (std::size_t t = k; t < gap_a.size(); ++t) d.calls.push_back({CallDiffKind::OnlyA, &xs[gap_a[t]], nullptr, {}, {}, {}, {}});
    for (std::size_t t = k; t < gap_b.size(); ++t) d.calls.push_back({CallDiffKind::OnlyB, nullptr, &ys[gap_b[t]], {}, {}, {}, {}});
    gap_a.clear();
    gap_b.clear();
  };
  std::size_t i = 0, j = 0;
  while (i < n && j < m) {
    if (id(xs[i]) == id(ys[j])) {
      flush();
      CallDiff c;
      c.a = &xs[i];
      c.b = &ys[j];
      const std::string oa = output_of(xs[i]), ob = output_of(ys[j]);
      if (oa == ob) {
        c.kind = CallDiffKind::Same;
      } else {
        c.kind = CallDiffKind::SameRequest;
        c.a_value = clip(oa);
        c.b_value = clip(ob);
      }
      d.calls.push_back(std::move(c));
      ++i;
      ++j;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      gap_a.push_back(i++);
    } else {
      gap_b.push_back(j++);
    }
  }
  while (i < n) gap_a.push_back(i++);
  while (j < m) gap_b.push_back(j++);
  flush();

  for (std::size_t k = 0; k < d.calls.size(); ++k) {
    if (d.calls[k].kind == CallDiffKind::Same) {
      ++d.same;
    } else if (d.first_divergence == SIZE_MAX) {
      d.first_divergence = k;
    }
  }

  // Workspace: at the end, and just before the first divergence.
  d.has_workspace = !a.snapshots.empty() && !b.snapshots.empty();
  if (d.has_workspace) {
    const auto* ea = a.snapshot_at(ws::When::RunEnd);
    const auto* eb = b.snapshot_at(ws::When::RunEnd);
    if (ea && eb) d.workspace_end = ws::diff(a.objects, ea->root, b.objects, eb->root);
    if (d.first_divergence != SIZE_MAX) {
      CallDiff& c = d.calls[d.first_divergence];
      if (c.a && c.b) {
        const auto* sa = a.snapshot_before(c.a->req.seq);
        const auto* sb = b.snapshot_before(c.b->req.seq);
        if (sa && sb) c.workspace_before = ws::diff(a.objects, sa->root, b.objects, sb->root);
      }
    }
  }

  for (const auto& e : xs) {
    const auto s = summarize(e);
    d.a_tokens += s.prompt_tokens + s.completion_tokens;
    if (e.resp && e.req.path != kToolPath) d.a_model_us += e.resp->latency_us;
  }
  for (const auto& e : ys) {
    const auto s = summarize(e);
    d.b_tokens += s.prompt_tokens + s.completion_tokens;
    if (e.resp && e.req.path != kToolPath) d.b_model_us += e.resp->latency_us;
  }
  return d;
}

}  // namespace khonsu::rec
