import { useEffect, useMemo, useRef, useState } from "react";
import type { TimelineInfo, World } from "../kernel/types";
import { ACTORS } from "../kernel/types";
import { ACTOR_COLORS, TAGS, digestColor, fmtVtime, laneColor } from "../lib/ui";

export interface Sel {
  tid: number;
  g: number;
}

interface Props {
  world: World;
  sel: Sel;
  onSelect: (s: Sel) => void;
}

const LANE_H = 38;
const TOP = 30;
const GUTTER = 150;
const RIGHT = 36;

/** Lane order: depth-first from main so forks sit directly under their parent. */
function laneOrder(tls: TimelineInfo[]): TimelineInfo[] {
  const kids = new Map<number, TimelineInfo[]>();
  for (const t of tls) {
    if (t.parent < 0) continue;
    const a = kids.get(t.parent) ?? [];
    a.push(t);
    kids.set(t.parent, a);
  }
  const out: TimelineInfo[] = [];
  const visit = (t: TimelineInfo) => {
    out.push(t);
    for (const c of kids.get(t.id) ?? []) visit(c);
  };
  for (const t of tls) if (t.parent < 0) visit(t);
  return out;
}

export function Multiverse({ world, sel, onSelect }: Props) {
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const [width, setWidth] = useState(900);
  const [hover, setHover] = useState<{ x: number; y: number; tid: number; g: number } | null>(null);
  const anim = useRef(new Map<number, { head: number; born: number }>());

  const lanes = useMemo(() => laneOrder(world.timelines), [world]);
  const laneIdx = useMemo(() => new Map(lanes.map((t, i) => [t.id, i])), [lanes]);
  const maxG = Math.max(18, ...world.timelines.map((t) => t.head + 1));
  const height = TOP + lanes.length * LANE_H + 18;

  useEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setWidth(el.clientWidth));
    ro.observe(el);
    setWidth(el.clientWidth);
    return () => ro.disconnect();
  }, []);

  const geom = useMemo(() => {
    const stepW = Math.max(14, (width - GUTTER - RIGHT) / maxG);
    const x = (g: number) => GUTTER + 12 + g * stepW;
    const y = (tid: number) => TOP + (laneIdx.get(tid) ?? 0) * LANE_H + LANE_H / 2;
    return { stepW, x, y };
  }, [width, maxG, laneIdx]);

  // Convergence: (g, digest) seen on more than one lane ⇒ identical world state.
  const converge = useMemo(() => {
    const m = new Map<string, number[]>();
    for (const t of world.timelines)
      for (const s of t.steps) {
        if (s.g === t.base && t.parent >= 0) continue;
        const k = `${s.g}:${s.digest}`;
        const a = m.get(k) ?? [];
        a.push(t.id);
        m.set(k, a);
      }
    return [...m.entries()].filter(([, v]) => v.length > 1).map(([k, v]) => ({ g: Number(k.split(":")[0]), tids: v }));
  }, [world]);

  useEffect(() => {
    const c = canvas.current;
    if (!c) return;
    const dpr = window.devicePixelRatio || 1;
    c.width = Math.round(width * dpr);
    c.height = Math.round(height * dpr);
    c.style.height = `${height}px`;
    const ctx = c.getContext("2d")!;
    let raf = 0;
    const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    const draw = (now: number) => {
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, width, height);
      const { x, y, stepW } = geom;
      let animating = false;

      // Step grid + axis.
      ctx.font = "10px JetBrains Mono, monospace";
      ctx.textAlign = "center";
      const every = stepW < 22 ? 5 : stepW < 40 ? 2 : 1;
      for (let g = 0; g < maxG; g++) {
        const gx = x(g);
        ctx.strokeStyle = g % 5 === 0 ? "rgba(42,52,80,.55)" : "rgba(30,38,56,.35)";
        ctx.beginPath();
        ctx.moveTo(gx, TOP - 6);
        ctx.lineTo(gx, height - 10);
        ctx.stroke();
        if (g % every === 0) {
          ctx.fillStyle = g === sel.g ? "#f5c26b" : "#5b6479";
          ctx.fillText(String(g), gx, 16);
        }
      }
      ctx.textAlign = "left";
      ctx.fillStyle = "#5b6479";
      ctx.fillText("step →", 14, 16);

      // Cursor.
      const cx = x(sel.g);
      const grad = ctx.createLinearGradient(cx - 18, 0, cx + 18, 0);
      grad.addColorStop(0, "rgba(245,194,107,0)");
      grad.addColorStop(0.5, "rgba(245,194,107,.10)");
      grad.addColorStop(1, "rgba(245,194,107,0)");
      ctx.fillStyle = grad;
      ctx.fillRect(cx - 18, TOP - 8, 36, height - TOP);
      ctx.strokeStyle = "rgba(245,194,107,.55)";
      ctx.setLineDash([3, 4]);
      ctx.beginPath();
      ctx.moveTo(cx, TOP - 8);
      ctx.lineTo(cx, height - 8);
      ctx.stroke();
      ctx.setLineDash([]);

      // Convergence ties.
      ctx.strokeStyle = "rgba(127,219,202,.35)";
      ctx.setLineDash([2, 3]);
      for (const cv of converge) {
        const ys = cv.tids.map(y);
        ctx.beginPath();
        ctx.moveTo(x(cv.g), Math.min(...ys));
        ctx.lineTo(x(cv.g), Math.max(...ys));
        ctx.stroke();
      }
      ctx.setLineDash([]);

      // Lanes.
      for (const t of lanes) {
        let a = anim.current.get(t.id);
        if (!a) {
          a = { head: t.base, born: now };
          anim.current.set(t.id, a);
        }
        a.head += (t.head - a.head) * (reduce ? 1 : 0.18);
        if (Math.abs(t.head - a.head) < 0.01) a.head = t.head;
        else animating = true;
        const born = reduce ? 1 : Math.min(1, (now - a.born) / 450);
        if (born < 1) animating = true;

        const color = laneColor(t.id);
        const ly = y(t.id);
        const dim = t.pruned ? 0.35 : 1;
        const selected = t.id === sel.tid;

        // Lane band + label.
        if (selected) {
          ctx.fillStyle = "rgba(245,194,107,.045)";
          ctx.fillRect(0, ly - LANE_H / 2 + 2, width, LANE_H - 4);
        }
        ctx.globalAlpha = dim;
        ctx.fillStyle = color;
        ctx.beginPath();
        ctx.arc(16, ly, 4, 0, Math.PI * 2);
        ctx.fill();
        ctx.font = `${selected ? 600 : 500} 12px Inter, sans-serif`;
        ctx.fillStyle = selected ? "#ffffff" : "#c9d1e0";
        ctx.fillText(t.name, 27, ly + 4);
        const badge = t.status === "done" ? "✓ done" : t.status === "failed" ? "✕ failed" : "… live";
        ctx.font = "600 10px Inter, sans-serif";
        ctx.fillStyle = t.status === "done" ? "#68d391" : t.status === "failed" ? "#ff6b6b" : "#8a94a7";
        ctx.fillText(t.pruned ? "pruned" : badge, 27 + Math.min(70, ctx.measureText(t.name).width + 42), ly + 4);
        ctx.globalAlpha = 1;

        // Clip for grow-in animation.
        ctx.save();
        const endX = x(a.head) + 10;
        ctx.beginPath();
        ctx.rect(GUTTER - 4, 0, Math.max(0, endX - GUTTER + 4), height);
        ctx.clip();
        ctx.globalAlpha = dim * (0.25 + 0.75 * born);

        // Fork curve from the parent's node.
        if (t.parent >= 0) {
          const py = y(t.parent);
          const x0 = x(t.base);
          const x1 = x0 + stepW * 0.7;
          ctx.strokeStyle = color;
          ctx.lineWidth = 2;
          ctx.beginPath();
          ctx.moveTo(x0, py);
          ctx.bezierCurveTo(x0 + stepW * 0.45, py, x0 + stepW * 0.2, ly, x1, ly);
          ctx.lineTo(x(Math.max(t.base + 1, Math.min(a.head, t.head))), ly);
          ctx.stroke();
        }
        // Spine.
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.shadowColor = color;
        ctx.shadowBlur = selected ? 8 : 0;
        ctx.beginPath();
        const first = t.parent >= 0 ? t.base + 1 : 0;
        if (t.head >= first) {
          ctx.moveTo(x(first), ly);
          ctx.lineTo(x(t.head), ly);
        }
        ctx.stroke();
        ctx.shadowBlur = 0;

        // Nodes.
        for (const s of t.steps) {
          if (t.parent >= 0 && s.g === t.base) continue;
          const nx = x(s.g);
          const tag = TAGS[s.tag];
          const big = tag && s.tag !== "cache_hit" && s.tag !== "code" && s.tag !== "plan_research";
          const r = big ? 7.5 : 4.5;
          ctx.beginPath();
          ctx.arc(nx, ly, r, 0, Math.PI * 2);
          ctx.fillStyle = s.live ? digestColor(s.digest) : "#2a3450";
          ctx.fill();
          ctx.lineWidth = 1.5;
          ctx.strokeStyle = big ? tag.color : "rgba(7,9,15,.9)";
          ctx.stroke();
          if (big) {
            ctx.fillStyle = "#07090f";
            ctx.font = "bold 9px Inter, sans-serif";
            ctx.textAlign = "center";
            ctx.fillText(tag.glyph, nx, ly + 3.2);
            ctx.textAlign = "left";
          }
          if (s.tag === "cache_hit" || s.saved > 0) {
            ctx.strokeStyle = "rgba(127,219,202,.9)";
            ctx.lineWidth = 1.2;
            ctx.beginPath();
            ctx.arc(nx, ly, r + 3, 0, Math.PI * 2);
            ctx.stroke();
          }
          // Actor tick under the node.
          ctx.fillStyle = ACTOR_COLORS[s.actor] ?? "#888";
          ctx.fillRect(nx - 3, ly + r + 3, 6, 2);
        }
        ctx.restore();

        // Head pulse for live timelines.
        if (!t.pruned && t.status === "running") {
          const p = reduce ? 0.5 : (Math.sin(now / 280) + 1) / 2;
          ctx.strokeStyle = `rgba(245,194,107,${0.25 + 0.5 * p})`;
          ctx.lineWidth = 1.5;
          ctx.beginPath();
          ctx.arc(x(Math.max(t.head, t.base + (t.parent >= 0 ? 0.7 : 0))), ly, 9 + 3 * p, 0, Math.PI * 2);
          ctx.stroke();
          animating = true;
        }
      }

      // Selection ring.
      const st = world.timelines[sel.tid];
      if (st) {
        const sx = x(sel.g);
        // A fork's base step is drawn on the parent's lane (it *is* the parent's node).
        let owner = st;
        while (owner.parent >= 0 && sel.g <= owner.base) owner = world.timelines[owner.parent];
        const sy = y(owner.id);
        ctx.strokeStyle = "#f5c26b";
        ctx.lineWidth = 2;
        ctx.shadowColor = "#f5c26b";
        ctx.shadowBlur = 14;
        ctx.beginPath();
        ctx.arc(sx, sy, 11, 0, Math.PI * 2);
        ctx.stroke();
        ctx.shadowBlur = 0;
      }

      if (animating) raf = requestAnimationFrame(draw);
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  }, [world, sel, width, height, geom, lanes, converge, maxG]);

  const pick = (ev: React.MouseEvent) => {
    const r = canvas.current!.getBoundingClientRect();
    const mx = ev.clientX - r.left;
    const my = ev.clientY - r.top;
    const lane = Math.floor((my - TOP) / LANE_H);
    const t = lanes[lane];
    if (!t) return null;
    const g = Math.round((mx - geom.x(0)) / geom.stepW);
    if (mx < GUTTER) return { x: mx, y: my, tid: t.id, g: t.head };
    const first = t.parent >= 0 ? t.base + 1 : 0;
    if (g < first - 1 || g > t.head) return null;
    return { x: mx, y: my, tid: t.id, g: Math.max(t.parent >= 0 ? t.base : 0, g) };
  };

  const hoverInfo = hover ? world.timelines[hover.tid] : null;
  const hoverStep = hoverInfo?.steps.find((s) => s.g === hover!.g);

  return (
    <div className="canvas-wrap" ref={wrap}>
      <canvas
        ref={canvas}
        style={{ cursor: hover ? "pointer" : "default" }}
        onMouseMove={(e) => setHover(pick(e))}
        onMouseLeave={() => setHover(null)}
        onClick={(e) => {
          const p = pick(e);
          if (p) onSelect({ tid: p.tid, g: p.g });
        }}
        role="img"
        aria-label="Timeline multiverse: one lane per timeline, one node per committed step"
      />
      {hover && hoverInfo && hoverStep && (
        <div className="tooltip" style={{ left: Math.min(hover.x + 14, width - 300), top: hover.y + 14 }}>
          <div style={{ display: "flex", gap: 6, alignItems: "center", marginBottom: 3 }}>
            <span className="dot" style={{ background: laneColor(hoverInfo.id) }} />
            <b>{hoverInfo.name}</b>
            <span className="t">step {hoverStep.g}</span>
            <span className="t" style={{ color: ACTOR_COLORS[hoverStep.actor] }}>{ACTORS[hoverStep.actor]}</span>
          </div>
          <div>{hoverStep.label}</div>
          <div className="t" style={{ marginTop: 3 }}>
            state {hoverStep.digest.slice(0, 12)} · t={fmtVtime(hoverStep.vtime)}
            {hoverStep.saved > 0 ? ` · ${hoverStep.saved} tokens from cache` : ""}
          </div>
        </div>
      )}
    </div>
  );
}

/** The timeline that owns global step g when viewed from `tid` (walks up past fork points). */
export function ancestorAt(world: World, tid: number, g: number): number {
  let t = world.timelines[tid];
  while (t && g < t.base && t.parent >= 0) t = world.timelines[t.parent];
  return t ? t.id : tid;
}
