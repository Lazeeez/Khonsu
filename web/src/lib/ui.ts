// Palette, formatting and small helpers shared by the views.

export const ACTOR_COLORS = [
  "#e2e8f0", // user
  "#a78bfa", // planner
  "#4fd1c5", // researcher
  "#f6ad55", // coder
  "#68d391", // tester
  "#63b3ed", // reviewer
  "#fc8181", // supervisor
];

// Lane hues for timelines: main is moon-silver, forks cycle through an even-hue set.
const LANE = ["#c9d1e0", "#f5c26b", "#7fdbca", "#b39dff", "#ff9e7a", "#7cc4ff", "#f78fb3", "#b5e36b", "#ffd57a", "#9ae6ff"];
export const laneColor = (id: number) => LANE[id % LANE.length];

/** Digest → stable hue. Equal states get equal colors across timelines. */
export function digestColor(digest: string, alpha = 1): string {
  const h = parseInt(digest.slice(0, 6), 16);
  const hue = h % 360;
  const light = 62 + ((h >> 9) % 10);
  return `hsla(${hue}, 70%, ${light}%, ${alpha})`;
}

export const TAGS: Record<string, { glyph: string; color: string; text: string }> = {
  genesis: { glyph: "◉", color: "#c9d1e0", text: "genesis" },
  fork: { glyph: "⑂", color: "#f5c26b", text: "fork point" },
  hallucination: { glyph: "⚠", color: "#ff6b6b", text: "hallucinated API" },
  tests_failed: { glyph: "✕", color: "#ff6b6b", text: "tests failed" },
  tests_passed: { glyph: "✓", color: "#68d391", text: "tests passed" },
  done: { glyph: "★", color: "#f5c26b", text: "task complete" },
  failed: { glyph: "■", color: "#ff6b6b", text: "task failed" },
  fault: { glyph: "ϟ", color: "#fbd38d", text: "chaos: actor killed" },
  restart: { glyph: "↻", color: "#68d391", text: "supervisor restart" },
  edit: { glyph: "✎", color: "#f5c26b", text: "user edit" },
  deferred: { glyph: "…", color: "#a0aec0", text: "deferred (actor down)" },
  cache_hit: { glyph: "⟲", color: "#7fdbca", text: "oracle cache hit" },
  plan_direct: { glyph: "→", color: "#fbd38d", text: "plan skips docs" },
  plan_research: { glyph: "📖", color: "#7fdbca", text: "plan reads docs" },
  code: { glyph: "⌨", color: "#68d391", text: "correct code" },
};

export function fmtVtime(us: number): string {
  if (us < 1000) return `${us} µs`;
  if (us < 1_000_000) return `${(us / 1000).toFixed(1)} ms`;
  return `${(us / 1_000_000).toFixed(2)} s`;
}

export function fmtNs(ns: number): string {
  if (ns < 10) return `${ns.toFixed(1)} ns`;
  if (ns < 1000) return `${ns.toFixed(0)} ns`;
  if (ns < 1_000_000) return `${(ns / 1000).toFixed(ns < 10_000 ? 2 : 1)} µs`;
  return `${(ns / 1_000_000).toFixed(2)} ms`;
}

export function fmtBytes(b: number): string {
  if (b < 1024) return `${b} B`;
  if (b < 1 << 20) return `${(b / 1024).toFixed(1)} KiB`;
  if (b < 1 << 30) return `${(b / (1 << 20)).toFixed(1)} MiB`;
  return `${(b / (1 << 30)).toFixed(2)} GiB`;
}

export const fmtInt = (n: number) => n.toLocaleString("en-US");

// ---- line diff (Myers-free LCS; files here are small) -------------------------------------

export type DiffLine = { t: " " | "+" | "-"; s: string };

export function lineDiff(a: string, b: string): DiffLine[] {
  const x = a.split("\n");
  const y = b.split("\n");
  // Trim common prefix/suffix first: generated files differ in one line.
  let pre = 0;
  while (pre < x.length && pre < y.length && x[pre] === y[pre]) pre++;
  let suf = 0;
  while (suf < x.length - pre && suf < y.length - pre && x[x.length - 1 - suf] === y[y.length - 1 - suf]) suf++;
  const xm = x.slice(pre, x.length - suf);
  const ym = y.slice(pre, y.length - suf);
  const n = xm.length, m = ym.length;
  const out: DiffLine[] = [];
  const ctx = 3;
  for (let i = Math.max(0, pre - ctx); i < pre; i++) out.push({ t: " ", s: x[i] });
  if (n * m > 4_000_000) {
    xm.forEach((s) => out.push({ t: "-", s }));
    ym.forEach((s) => out.push({ t: "+", s }));
  } else {
    const dp: Uint32Array[] = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
    for (let i = n - 1; i >= 0; i--)
      for (let j = m - 1; j >= 0; j--)
        dp[i][j] = xm[i] === ym[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
    let i = 0, j = 0;
    while (i < n && j < m) {
      if (xm[i] === ym[j]) { out.push({ t: " ", s: xm[i] }); i++; j++; }
      else if (dp[i + 1][j] >= dp[i][j + 1]) out.push({ t: "-", s: xm[i++] });
      else out.push({ t: "+", s: ym[j++] });
    }
    while (i < n) out.push({ t: "-", s: xm[i++] });
    while (j < m) out.push({ t: "+", s: ym[j++] });
  }
  for (let k = x.length - suf; k < Math.min(x.length, x.length - suf + ctx); k++) out.push({ t: " ", s: x[k] });
  return out;
}

export function download(name: string, data: BlobPart, type = "application/octet-stream") {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
