// The viewer's Examples dropdown: every bundled recording in one menu, with what each shows.
import { useEffect, useRef, useState } from "react";

export interface Sample {
  file: string;
  title: string;
  blurb: string;
  summary?: string;
  story?: { call?: number; text: string }[];
}

export function Examples({ samples, current, disabled, onPick }: {
  samples: Sample[];
  current?: string;
  disabled?: boolean;
  onPick: (s: Sample) => void;
}) {
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const onDown = (e: PointerEvent) => box.current && !box.current.contains(e.target as Node) && setOpen(false);
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
    addEventListener("pointerdown", onDown);
    addEventListener("keydown", onKey);
    return () => {
      removeEventListener("pointerdown", onDown);
      removeEventListener("keydown", onKey);
    };
  }, [open]);
  if (!samples.length) return null;
  return (
    <div className="examples" ref={box}>
      <button className="btn" onClick={() => setOpen(!open)} disabled={disabled} aria-haspopup="menu" aria-expanded={open}>
        Examples <span className={`caret ${open ? "up" : ""}`} aria-hidden>▾</span>
      </button>
      {open && (
        <div className="ex-menu" role="menu">
          {samples.map((s) => (
            <button key={s.file} role="menuitem" className={`ex-item ${current === s.file ? "on" : ""}`}
              onClick={() => { setOpen(false); onPick(s); }}>
              <b>{s.title}</b>
              <span>{s.blurb}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
