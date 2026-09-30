// The night sky behind every page, on two canvases so it costs almost nothing:
//  - stars: slow twinkle and scroll parallax, redrawn at ~20 fps (twinkling is slow);
//  - meteors: frequent shooting stars and the occasional burst, drawn every frame, but
//    only while one is in flight (an idle frame is a single time check).
// Paused in hidden tabs; drawn once, with no meteors, for prefers-reduced-motion.
import { useEffect, useRef } from "react";
import { reducedMotion } from "../lib/motion";

interface Star { x: number; y: number; r: number; a: number; tw: number; ph: number; depth: number; c: string }
interface Meteor { x: number; y: number; vx: number; vy: number; t0: number; life: number; len: number; w: number; bright: number }

const COLORS = ["255,255,255", "255,244,214", "233,185,73", "140,220,210"];
const STAR_FRAME_MS = 50;

function makeStars(w: number, h: number): Star[] {
  const n = Math.min(520, Math.round((w * h) / 4000));
  let seed = 7;
  const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  return Array.from({ length: n }, () => {
    const depth = 0.25 + rnd() * 0.75;
    const bright = rnd() < 0.06;
    return {
      x: rnd() * w,
      y: rnd() * h,
      r: (bright ? 1.1 : 0.45) + rnd() * (bright ? 0.9 : 0.7) * depth,
      a: (bright ? 0.75 : 0.25) + rnd() * 0.45,
      tw: 0.4 + rnd() * 1.4,
      ph: rnd() * Math.PI * 2,
      depth,
      c: COLORS[rnd() < 0.72 ? 0 : rnd() < 0.6 ? 1 : rnd() < 0.7 ? 2 : 3],
    };
  });
}

export function Sky() {
  const starsRef = useRef<HTMLCanvasElement>(null);
  const meteorsRef = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const sc = starsRef.current!, mc = meteorsRef.current!;
    const sg = sc.getContext("2d")!, mg = mc.getContext("2d")!;
    const dpr = Math.min(1.5, devicePixelRatio || 1);
    let w = 0, h = 0, stars: Star[] = [];
    const resize = () => {
      w = innerWidth;
      h = innerHeight;
      for (const [c, g] of [[sc, sg], [mc, mg]] as const) {
        c.width = Math.round(w * dpr);
        c.height = Math.round(h * dpr);
        g.setTransform(dpr, 0, 0, dpr, 0, 0);
      }
      stars = makeStars(w, h);
    };
    resize();
    const still = reducedMotion();

    const drawStars = (now: number) => {
      sg.clearRect(0, 0, w, h);
      const t = now / 1000, sy = scrollY;
      for (const s of stars) {
        const tw = still ? 0.8 : 0.62 + 0.38 * Math.sin(t * s.tw + s.ph);
        const y = (((s.y - sy * 0.06 * s.depth) % h) + h) % h;
        sg.fillStyle = `rgba(${s.c},${(s.a * tw).toFixed(3)})`;
        if (s.r < 0.9) {
          sg.fillRect(s.x, y, s.r * 1.6, s.r * 1.6);
        } else {
          sg.beginPath();
          sg.arc(s.x, y, s.r, 0, Math.PI * 2);
          sg.fill();
          if (s.r > 1.4) {
            sg.fillStyle = `rgba(${s.c},${(0.08 * tw).toFixed(3)})`;
            sg.beginPath();
            sg.arc(s.x, y, s.r * 4, 0, Math.PI * 2);
            sg.fill();
          }
        }
      }
    };

    const meteors: Meteor[] = [];
    let next = performance.now() + 700;
    let nextBurst = performance.now() + 9000 + Math.random() * 9000;
    let meteorCanvasDirty = false;
    const spawn = (now: number, delay = 0) => {
      const ang = (Math.PI / 180) * (14 + Math.random() * 30);
      const dir = Math.random() < 0.8 ? 1 : -1;  // mostly left-to-right, some the other way
      const sp = 0.7 + Math.random() * 0.9;
      const bright = Math.random() < 0.25 ? 1 : 0.55 + Math.random() * 0.3;
      meteors.push({
        x: dir > 0 ? w * (Math.random() * 0.75) : w * (0.25 + Math.random() * 0.75),
        y: h * Math.random() * 0.45,
        vx: Math.cos(ang) * sp * dir,
        vy: Math.sin(ang) * sp,
        t0: now + delay,
        life: 700 + Math.random() * 900,
        len: 110 + Math.random() * 190,
        w: 0.9 + bright * 1.1,
        bright,
      });
    };
    const drawMeteors = (now: number) => {
      if (now > next) {
        if (meteors.length < 3) spawn(now);
        next = now + 1000 + Math.random() * 2500;
      }
      if (now > nextBurst) {  // a small meteor shower: three in quick succession
        for (let i = 0; i < 3; i++) spawn(now, i * (160 + Math.random() * 220));
        nextBurst = now + 14000 + Math.random() * 12000;
      }
      if (!meteors.some((m) => now >= m.t0)) {
        if (meteorCanvasDirty) {
          mg.clearRect(0, 0, w, h);
          meteorCanvasDirty = false;
        }
        return;
      }
      mg.clearRect(0, 0, w, h);
      meteorCanvasDirty = true;
      mg.lineCap = "round";
      for (let i = meteors.length - 1; i >= 0; i--) {
        const m = meteors[i];
        const age = now - m.t0;
        if (age < 0) continue;
        if (age > m.life) {
          meteors.splice(i, 1);
          continue;
        }
        const k = age / m.life;
        const fade = Math.sin(Math.PI * Math.min(1, k * 1.15)) * m.bright;
        const hx = m.x + m.vx * age, hy = m.y + m.vy * age;
        const norm = Math.hypot(m.vx, m.vy) || 1;
        const tail = m.len * (0.35 + 0.65 * Math.min(1, k * 3));  // the trail grows, then fades
        const tx = hx - (m.vx / norm) * tail, ty = hy - (m.vy / norm) * tail;
        const grad = mg.createLinearGradient(hx, hy, tx, ty);
        grad.addColorStop(0, `rgba(255,248,226,${0.95 * fade})`);
        grad.addColorStop(0.25, `rgba(233,205,140,${0.45 * fade})`);
        grad.addColorStop(1, "rgba(160,190,255,0)");
        mg.strokeStyle = grad;
        mg.lineWidth = m.w;
        mg.beginPath();
        mg.moveTo(hx, hy);
        mg.lineTo(tx, ty);
        mg.stroke();
        const head = mg.createRadialGradient(hx, hy, 0, hx, hy, 6 * m.w);  // a glowing head
        head.addColorStop(0, `rgba(255,250,235,${0.9 * fade})`);
        head.addColorStop(1, "rgba(255,250,235,0)");
        mg.fillStyle = head;
        mg.beginPath();
        mg.arc(hx, hy, 6 * m.w, 0, Math.PI * 2);
        mg.fill();
      }
    };

    let raf = 0, lastStars = -1e9;
    const loop = (now: number) => {
      raf = requestAnimationFrame(loop);
      if (document.hidden) return;
      if (now - lastStars >= STAR_FRAME_MS) {
        lastStars = now;
        drawStars(now);
      }
      drawMeteors(now);
    };
    const onResize = () => {
      resize();
      drawStars(performance.now());
    };
    addEventListener("resize", onResize);
    if (still) drawStars(performance.now());
    else raf = requestAnimationFrame(loop);
    return () => {
      cancelAnimationFrame(raf);
      removeEventListener("resize", onResize);
    };
  }, []);
  return (
    <>
      <canvas ref={starsRef} className="sky" aria-hidden />
      <canvas ref={meteorsRef} className="sky" aria-hidden />
    </>
  );
}
