// The landing page's dial, running a story on its own: the traveller glides from call
// to call through a run, lingers where the tests failed, then sweeps back to the start,
// travelling back in time. Touch it and it becomes an ordinary dial.
import { useEffect, useState } from "react";
import { easeInOutCubic, reducedMotion } from "../lib/motion";
import { MoonDial, type DialTick } from "../viewer/MoonDial";

const GLIDE = 620, DWELL = 520, FAIL_DWELL = 1500, HOLD_END = 900, REWIND = 2300, HOLD_START = 700;

export function HeroDial({ ticks, size = 400 }: { ticks: DialTick[]; size?: number }) {
  const n = ticks.length;
  const [pos, setPos] = useState(0);
  const [rewinding, setRewinding] = useState(false);
  const [manual, setManual] = useState<number | null>(null);

  useEffect(() => {
    if (manual !== null || reducedMotion() || n < 2) return;
    // Timeline: [glide, dwell] per call, a hold at the end, the rewind, a hold at the start.
    const segs: { start: number; end: number; from: number; to: number }[] = [];
    let t = 0;
    for (let i = 0; i < n - 1; i++) {
      segs.push({ start: t, end: t + GLIDE, from: i, to: i + 1 });
      t += GLIDE + (ticks[i + 1].flagged ? FAIL_DWELL : DWELL);
    }
    t += HOLD_END - DWELL;
    const rewindAt = t;
    segs.push({ start: t, end: t + REWIND, from: n - 1, to: 0 });
    const total = t + REWIND + HOLD_START;

    let raf = 0;
    const t0 = performance.now();
    const loop = (now: number) => {
      raf = requestAnimationFrame(loop);
      if (document.hidden) return;
      const at = (now - t0) % total;
      let p = at >= rewindAt + REWIND ? 0 : n - 1;
      for (const s of segs) {
        if (at < s.start) { p = s.from; break; }
        if (at <= s.end) { p = s.from + (s.to - s.from) * easeInOutCubic((at - s.start) / (s.end - s.start)); break; }
        p = s.to;
      }
      setPos(p);
      setRewinding(at >= rewindAt && at < rewindAt + REWIND);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [manual, n, ticks]);

  const i = manual ?? Math.round(pos);
  const t = ticks[i];
  const caption = rewinding
    ? "⟲ travelling back to the start"
    : `${t.kind === "tool" ? "tool" : "model"} · ${t.label}${t.flagged ? "  ✕ tests failed here" : ""}`;
  return (
    <MoonDial ticks={ticks} value={i} position={manual === null ? pos : undefined} size={size}
      onChange={setManual} caption={caption} />
  );
}
