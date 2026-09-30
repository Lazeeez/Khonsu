// Motion primitives. Everything animates transform/opacity or a single number, eases
// with frame-rate-independent springs, and honours prefers-reduced-motion.
import { useEffect, useRef, useState, type RefObject } from "react";

export const reducedMotion = () =>
  typeof matchMedia !== "undefined" && matchMedia("(prefers-reduced-motion: reduce)").matches;

export const easeInOutCubic = (t: number) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
export const easeOutCubic = (t: number) => 1 - Math.pow(1 - t, 3);
export const easeOutExpo = (t: number) => (t >= 1 ? 1 : 1 - Math.pow(2, -10 * t));

/** A number that glides to `target` (critically damped, time constant `tau` ms). */
export function useSmoothed(target: number, tau = 120): number {
  const [value, setValue] = useState(target);
  const cur = useRef(target);
  useEffect(() => {
    if (reducedMotion()) {
      cur.current = target;
      setValue(target);
      return;
    }
    let raf = 0;
    let last = performance.now();
    const tick = (now: number) => {
      const dt = Math.min(64, now - last);
      last = now;
      const next = cur.current + (target - cur.current) * (1 - Math.exp(-dt / tau));
      const done = Math.abs(target - next) < 1e-3;
      cur.current = done ? target : next;
      setValue(cur.current);
      if (!done) raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [target, tau]);
  return value;
}

/** Adds `in` to every `.reveal` element under `root` as it scrolls into view (once). */
export function useReveal(root: RefObject<HTMLElement | null>) {
  useEffect(() => {
    const el = root.current;
    if (!el) return;
    const items = [...el.querySelectorAll<HTMLElement>(".reveal")];
    if (reducedMotion() || typeof IntersectionObserver === "undefined") {
      items.forEach((i) => i.classList.add("in"));
      return;
    }
    el.classList.add("reveal-ready");  // content is hidden only once something will reveal it
    const io = new IntersectionObserver(
      (entries) => {
        for (const e of entries)
          if (e.isIntersecting) {
            e.target.classList.add("in");
            io.unobserve(e.target);
          }
      },
      { rootMargin: "0px 0px -8% 0px", threshold: 0.12 },
    );
    items.forEach((i) => io.observe(i));
    return () => io.disconnect();
  });
}

/** Counts from 0 to `to` once `start` becomes true. */
export function useCountUp(to: number, start: boolean, ms = 1400): number {
  const [v, setV] = useState(0);
  useEffect(() => {
    if (!start) return;
    if (reducedMotion()) {
      setV(to);
      return;
    }
    let raf = 0;
    const t0 = performance.now();
    const tick = (now: number) => {
      const t = Math.min(1, (now - t0) / ms);
      setV(to * easeOutExpo(t));
      if (t < 1) raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [to, start, ms]);
  return v;
}

/** Sets --mx/--my on an element under the pointer, for the card spotlight. */
export function spotlight(e: React.PointerEvent<HTMLElement>) {
  const r = e.currentTarget.getBoundingClientRect();
  e.currentTarget.style.setProperty("--mx", `${e.clientX - r.left}px`);
  e.currentTarget.style.setProperty("--my", `${e.clientY - r.top}px`);
}
