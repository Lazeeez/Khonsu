import { useCallback, useRef } from "react";

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
}

/** Lit crescent/gibbous path for illuminated fraction f (waxing, lit from the right). */
function moonPath(cx: number, cy: number, r: number, f: number): string {
  const rx = r * Math.abs(2 * f - 1);
  const sweep = f > 0.5 ? 1 : 0;
  return `M ${cx} ${cy - r} A ${r} ${r} 0 0 1 ${cx} ${cy + r} A ${rx} ${r} 0 0 ${sweep} ${cx} ${cy - r} Z`;
}

/**
 * The Moon Dial: the run is an orbit, each call a point on it; the moon in the
 * middle waxes as you travel from the first call to the last. Drag, click, use
 * arrow keys / Home / End, or scroll.
 */
export function MoonDial({ ticks, value, onChange, size = 300, caption }: Props) {
  const svg = useRef<SVGSVGElement>(null);
  const n = Math.max(ticks.length, 1);
  const c = size / 2;
  const R = size / 2 - 22;
  const gap = n > 1 ? 0.06 : 0;  // a small opening at the top marks start/end
  const angleOf = (i: number) => -Math.PI / 2 + gap * Math.PI + (n <= 1 ? 0 : (i / (n - 1)) * (2 * Math.PI * (1 - gap)));
  const pos = (i: number, r = R) => [c + r * Math.cos(angleOf(i)), c + r * Math.sin(angleOf(i))] as const;

  const pick = useCallback(
    (clientX: number, clientY: number) => {
      const box = svg.current!.getBoundingClientRect();
      const x = ((clientX - box.left) / box.width) * size - c;
      const y = ((clientY - box.top) / box.height) * size - c;
      let a = Math.atan2(y, x) + Math.PI / 2 - gap * Math.PI;  // 0 at the first tick
      if (a < 0) a += 2 * Math.PI;
      const t = Math.min(1, Math.max(0, a / (2 * Math.PI * (1 - gap))));
      onChange(Math.round(t * (n - 1)));
    },
    [c, gap, n, onChange, size],
  );

  const f = n <= 1 ? 1 : (value + 1) / n;
  const [hx, hy] = pos(value);
  const cur = ticks[value];
  const moonR = R * 0.46;

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
          <radialGradient id="moonlit" cx="0.62" cy="0.38" r="0.75">
            <stop offset="0" stopColor="#fff6dc" />
            <stop offset="0.55" stopColor="#f1cf7e" />
            <stop offset="1" stopColor="#c8952f" />
          </radialGradient>
          <radialGradient id="halo" cx="0.5" cy="0.5" r="0.5">
            <stop offset="0.55" stopColor="rgba(241,207,126,0.22)" />
            <stop offset="1" stopColor="rgba(241,207,126,0)" />
          </radialGradient>
          <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur stdDeviation="2.4" result="b" />
            <feMerge><feMergeNode in="b" /><feMergeNode in="SourceGraphic" /></feMerge>
          </filter>
        </defs>

        {/* orbit + progress arc */}
        <circle cx={c} cy={c} r={R} fill="none" stroke="var(--line-2)" strokeWidth="1.5" strokeDasharray="2 5" />
        {n > 1 && value > 0 && (
          <path
            d={(() => {
              const [x0, y0] = pos(0);
              const large = angleOf(value) - angleOf(0) > Math.PI ? 1 : 0;
              return `M ${x0} ${y0} A ${R} ${R} 0 ${large} 1 ${hx} ${hy}`;
            })()}
            fill="none" stroke="var(--gold)" strokeWidth="2.5" strokeLinecap="round" opacity="0.85"
          />
        )}

        {/* one mark per call */}
        {ticks.map((t, i) => {
          const [x, y] = pos(i);
          const color = t.flagged ? "var(--bad)" : t.kind === "tool" ? "var(--faience)" : "var(--gold)";
          return (
            <g key={i} onPointerDown={(e) => { e.stopPropagation(); onChange(i); }} style={{ cursor: "pointer" }}>
              <circle cx={x} cy={y} r={10} fill="transparent" />
              <circle cx={x} cy={y} r={i === value ? 0 : t.flagged ? 4.5 : 3.4}
                fill={t.replayed ? "var(--bg)" : color} stroke={color} strokeWidth={t.replayed ? 1.6 : 0} />
            </g>
          );
        })}

        {/* the traveller: current position on the orbit */}
        <circle cx={hx} cy={hy} r={9} fill="url(#moonlit)" filter="url(#glow)" />

        {/* the moon, waxing with progress */}
        <circle cx={c} cy={c} r={moonR * 1.55} fill="url(#halo)" />
        <circle cx={c} cy={c} r={moonR} fill="#0e1a38" stroke="rgba(241,207,126,0.25)" />
        <path d={moonPath(c, c, moonR, f)} fill="url(#moonlit)" />
        <text x={c} y={c + moonR + 30} textAnchor="middle" className="dial-step">
          {n === 0 ? "—" : `call ${value} of ${n - 1}`}
        </text>
      </svg>
      {caption && <div className="dial-caption">{caption}</div>}
    </div>
  );
}
