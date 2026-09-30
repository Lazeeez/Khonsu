import { memo, useCallback, useId, useRef } from "react";
import { useSmoothed } from "../lib/motion";

export interface DialTick {
  kind: "model" | "tool";
  replayed?: boolean;
  flagged?: boolean;  // divergence / error / annotation
  label: string;
}

interface Props {
  ticks: DialTick[];
  value: number;
  onChange: (i: number) => void;
  size?: number;
  caption?: string;
  /** A continuous position to show instead of easing to `value` (the landing page drives it). */
  position?: number;
}

/** Lit crescent/gibbous path for illuminated fraction f (waxing, lit from the right). */
function moonPath(cx: number, cy: number, r: number, f: number): string {
  const rx = r * Math.abs(2 * f - 1);
  const sweep = f > 0.5 ? 1 : 0;
  return `M ${cx} ${cy - r} A ${r} ${r} 0 0 1 ${cx} ${cy + r} A ${rx} ${r} 0 0 ${sweep} ${cx} ${cy - r} Z`;
}

interface Geometry {
  c: number;
  R: number;
  angleOf: (x: number) => number;
  pos: (x: number, r?: number) => readonly [number, number];
}

function geometry(size: number, n: number): Geometry {
  const c = size / 2;
  const R = size / 2 - 22;
  const gap = n > 1 ? 0.06 : 0;  // a small opening at the top marks start/end
  const angleOf = (x: number) => -Math.PI / 2 + gap * Math.PI + (n <= 1 ? 0 : (x / (n - 1)) * (2 * Math.PI * (1 - gap)));
  const pos = (x: number, r = R) => [c + r * Math.cos(angleOf(x)), c + r * Math.sin(angleOf(x))] as const;
  return { c, R, angleOf, pos };
}

/** The call marks: re-rendered only when the ticks or the selection change, not every frame. */
const Ticks = memo(function Ticks({ ticks, size, value, onChange }: { ticks: DialTick[]; size: number; value: number; onChange: (i: number) => void }) {
  const { pos } = geometry(size, Math.max(ticks.length, 1));
  return (
    <g>
      {ticks.map((t, i) => {
        const [x, y] = pos(i);
        const color = t.flagged ? "var(--bad)" : t.kind === "tool" ? "var(--faience)" : "var(--gold)";
        return (
          <g key={i} onPointerDown={(e) => { e.stopPropagation(); onChange(i); }} style={{ cursor: "pointer" }}>
            <circle cx={x} cy={y} r={10} fill="transparent" />
            <circle className={`dial-tick ${i === value ? "at" : ""}`} cx={x} cy={y} r={t.flagged ? 4.5 : 3.4}
              fill={t.replayed ? "var(--bg)" : color} stroke={color} strokeWidth={t.replayed ? 1.6 : 0}
              />
          </g>
        );
      })}
    </g>
  );
});

/**
 * The Moon Dial: the run is an orbit, each call a point on it; the moon in the
 * middle waxes as you travel from the first call to the last. Drag, click, use
 * arrow keys / Home / End, or scroll. Every change glides; nothing jumps.
 */
export function MoonDial({ ticks, value, onChange, size = 300, caption, position }: Props) {
  const svg = useRef<SVGSVGElement>(null);
  const uid = useId().replace(/:/g, "");
  const n = Math.max(ticks.length, 1);
  const { c, R, angleOf, pos } = geometry(size, n);
  const eased = useSmoothed(value, 140);
  const shown = Math.min(n - 1, Math.max(0, position ?? eased));

  const pick = useCallback(
    (clientX: number, clientY: number) => {
      const box = svg.current!.getBoundingClientRect();
      const x = ((clientX - box.left) / box.width) * size - c;
      const y = ((clientY - box.top) / box.height) * size - c;
      const gap = n > 1 ? 0.06 : 0;
      let a = Math.atan2(y, x) + Math.PI / 2 - gap * Math.PI;  // 0 at the first tick
      if (a < 0) a += 2 * Math.PI;
      const t = Math.min(1, Math.max(0, a / (2 * Math.PI * (1 - gap))));
      onChange(Math.round(t * (n - 1)));
    },
    [c, n, onChange, size],
  );

  const f = n <= 1 ? 1 : 0.08 + 0.92 * (shown / (n - 1));
  const [hx, hy] = pos(shown);
  const cur = ticks[value];
  const moonR = R * 0.46;
  const [x0, y0] = pos(0);
  const large = angleOf(shown) - angleOf(0) > Math.PI ? 1 : 0;
  const id = (s: string) => `${s}-${uid}`;

  return (
    <div className="moondial">
      <svg
        ref={svg}
        viewBox={`0 0 ${size} ${size}`}
        role="slider"
        tabIndex={0}
        aria-label="Moon Dial: travel through the run's calls"
        aria-valuemin={0}
        aria-valuemax={n - 1}
        aria-valuenow={value}
        aria-valuetext={cur ? `call ${value}: ${cur.label}` : "no calls"}
        onPointerDown={(e) => {
          (e.target as Element).setPointerCapture?.(e.pointerId);
          pick(e.clientX, e.clientY);
        }}
        onPointerMove={(e) => e.buttons === 1 && pick(e.clientX, e.clientY)}
        onWheel={(e) => onChange(Math.min(n - 1, Math.max(0, value + (e.deltaY > 0 ? 1 : -1))))}
        onKeyDown={(e) => {
          const next = e.key === "ArrowRight" || e.key === "ArrowDown" ? value + 1
            : e.key === "ArrowLeft" || e.key === "ArrowUp" ? value - 1
            : e.key === "Home" ? 0 : e.key === "End" ? n - 1 : null;
          if (next !== null) {
            e.preventDefault();
            onChange(Math.min(n - 1, Math.max(0, next)));
          }
        }}
      >
        <defs>
          <radialGradient id={id("moonlit")} cx="0.62" cy="0.38" r="0.75">
            <stop offset="0" stopColor="#fff6dc" />
            <stop offset="0.55" stopColor="#f1cf7e" />
            <stop offset="1" stopColor="#c8952f" />
          </radialGradient>
          <radialGradient id={id("dark")} cx="0.4" cy="0.35" r="0.8">
            <stop offset="0" stopColor="#13234a" />
            <stop offset="1" stopColor="#081330" />
          </radialGradient>
          <radialGradient id={id("halo")} cx="0.5" cy="0.5" r="0.5">
            <stop offset="0.5" stopColor="rgba(241,207,126,0.20)" />
            <stop offset="1" stopColor="rgba(241,207,126,0)" />
          </radialGradient>
          <filter id={id("glow")} x="-80%" y="-80%" width="260%" height="260%">
            <feGaussianBlur stdDeviation="3.2" result="b" />
            <feMerge><feMergeNode in="b" /><feMergeNode in="SourceGraphic" /></feMerge>
          </filter>
        </defs>

        {/* orbit + progress arc */}
        <circle cx={c} cy={c} r={R} fill="none" stroke="var(--line-2)" strokeWidth="1.2" strokeDasharray="1.5 5.5" />
        {n > 1 && shown > 0.001 && (
          <path d={`M ${x0} ${y0} A ${R} ${R} 0 ${large} 1 ${hx} ${hy}`}
            fill="none" stroke="var(--gold)" strokeWidth="2.4" strokeLinecap="round" opacity="0.9" filter={`url(#${id("glow")})`} />
        )}

        <Ticks ticks={ticks} size={size} value={value} onChange={onChange} />

        {/* the moon, waxing with progress */}
        <circle className="dial-halo" cx={c} cy={c} r={moonR * 1.7} fill={`url(#${id("halo")})`} />
        <circle cx={c} cy={c} r={moonR} fill={`url(#${id("dark")})`} stroke="rgba(241,207,126,0.22)" />
        <path d={moonPath(c, c, moonR, f)} fill={`url(#${id("moonlit")})`} />

        {/* the traveller: current position on the orbit */}
        <circle cx={hx} cy={hy} r={16} fill="rgba(241,207,126,0.16)" />
        <circle cx={hx} cy={hy} r={8.5} fill={`url(#${id("moonlit")})`} filter={`url(#${id("glow")})`} />

        <text x={c} y={c + moonR + 30} textAnchor="middle" className="dial-step">
          {n === 0 ? "—" : `call ${Math.round(shown)} of ${n - 1}`}
        </text>
      </svg>
      {caption && <div className="dial-caption">{caption}</div>}
    </div>
  );
}
