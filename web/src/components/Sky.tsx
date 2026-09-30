// The night sky behind every page: a canvas star field with slow twinkling, depth
// parallax on scroll, and a rare shooting star. Capped at ~40 fps, paused in hidden
// tabs, and drawn once (static) for reduced motion or when `animate` is false.
import { useEffect, useRef } from "react";
import { reducedMotion } from "../lib/motion";

interface Star { x: number; y: number; r: number; a: number; tw: number; ph: number; depth: number; c: string }

const COLORS = ["255,255,255", "255,244,214", "233,185,73", "140,220,210"];

function makeStars(w: number, h: number): Star[] {
  const n = Math.min(460, Math.round((w * h) / 4600));
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

export function Sky({ animate }: { animate: boolean }) {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const canvas = ref.current!;
    const g = canvas.getContext("2d")!;
    const dpr = Math.min(1.5, devicePixelRatio || 1);
    let w = 0, h = 0, stars: Star[] = [];
    const resize = () => {
      w = innerWidth;
      h = innerHeight;
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      stars = makeStars(w, h);
    };
    resize();

    const still = !animate || reducedMotion();
    let shoot: { x: number; y: number; vx: number; vy: number; t0: number } | null = null;
    let nextShoot = performance.now() + 4000 + Math.random() * 6000;

    const draw = (now: number) => {
      g.clearRect(0, 0, w, h);
      const t = now / 1000;
      const sy = scrollY;
      for (const s of stars) {
        const tw = still ? 0.8 : 0.62 + 0.38 * Math.sin(t * s.tw + s.ph);
        const y = (((s.y - sy * 0.06 * s.depth) % h) + h) % h;
        g.fillStyle = `rgba(${s.c},${(s.a * tw).toFixed(3)})`;
        if (s.r < 0.9) {
          g.fillRect(s.x, y, s.r * 1.6, s.r * 1.6);
        } else {
          g.beginPath();
          g.arc(s.x, y, s.r, 0, Math.PI * 2);
          g.fill();
          if (s.r > 1.4) {  // a soft halo on the brightest stars
            g.fillStyle = `rgba(${s.c},${(0.08 * tw).toFixed(3)})`;
            g.beginPath();
            g.arc(s.x, y, s.r * 4, 0, Math.PI * 2);
            g.fill();
          }
        }
      }
      if (still) return;
      if (!shoot && now > nextShoot) {
        const ang = (Math.PI / 180) * (18 + Math.random() * 20);
        const sp = 0.9 + Math.random() * 0.5;
        shoot = { x: w * (0.15 + Math.random() * 0.6), y: h * Math.random() * 0.35, vx: Math.cos(ang) * sp, vy: Math.sin(ang) * sp, t0: now };
      }
      if (shoot) {
        const age = now - shoot.t0;
        const life = 1100;
        if (age > life) {
          shoot = null;
          nextShoot = now + 7000 + Math.random() * 9000;
        } else {
          const k = age / life;
          const fade = Math.sin(Math.PI * k);
          const hx = shoot.x + shoot.vx * age, hy = shoot.y + shoot.vy * age;
          const tx = hx - shoot.vx * 160, ty = hy - shoot.vy * 160;
          const grad = g.createLinearGradient(hx, hy, tx, ty);
          grad.addColorStop(0, `rgba(255,244,214,${0.9 * fade})`);
          grad.addColorStop(1, "rgba(255,244,214,0)");
          g.strokeStyle = grad;
          g.lineWidth = 1.4;
          g.lineCap = "round";
          g.beginPath();
          g.moveTo(hx, hy);
          g.lineTo(tx, ty);
          g.stroke();
        }
      }
    };

    let raf = 0, last = 0;
    const loop = (now: number) => {
      raf = requestAnimationFrame(loop);
      if (document.hidden || now - last < 24) return;  // ~40 fps is plenty for a sky
      last = now;
      draw(now);
    };
    const onResize = () => {
      resize();
      draw(performance.now());
    };
    addEventListener("resize", onResize);
    if (still) {
      draw(performance.now());
    } else {
      raf = requestAnimationFrame(loop);
    }
    return () => {
      cancelAnimationFrame(raf);
      removeEventListener("resize", onResize);
    };
  }, [animate]);
  return <canvas ref={ref} className="sky" aria-hidden />;
}
