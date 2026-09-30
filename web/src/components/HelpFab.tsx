// The floating "?" in the lower-right corner of every tool page. Tap or click it for the
// page's how-to (and, in the viewer, the story of the example that is open). Closes on
// Escape, on an outside click, or with its close button.
import { useEffect, useId, useRef, useState } from "react";

export interface ExampleStory {
  title: string;
  summary: string;
  story: { call?: number; text: string }[];
}

export function HelpFab({ title, steps, guide, example, onGoto }: {
  title: string;
  steps: React.ReactNode[];
  guide: string;                        // Guide section id (#/guide?s=...)
  example?: ExampleStory | null;        // the open example, if any
  onGoto?: (call: number) => void;      // jump to a call from the story
}) {
  const [open, setOpen] = useState(false);
  const [nudge, setNudge] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const id = useId();

  // A new example: pulse and hint once, without opening on its own.
  useEffect(() => {
    if (!example) return;
    setNudge(true);
    const t = setTimeout(() => setNudge(false), 5200);
    return () => clearTimeout(t);
  }, [example]);

  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
    const onDown = (e: PointerEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) setOpen(false);
    };
    addEventListener("keydown", onKey);
    addEventListener("pointerdown", onDown);
    return () => {
      removeEventListener("keydown", onKey);
      removeEventListener("pointerdown", onDown);
    };
  }, [open]);

  const label = example ? "What happened in this example, and how to use this page" : `How to use ${title}`;
  return (
    <div className="fab" ref={box}>
      {open && (
        <div className="fab-panel" id={id} role="dialog" aria-label={label}>
          <button className="fab-close" onClick={() => setOpen(false)} aria-label="Close">×</button>
          {example && (
            <section className="fab-sec">
              <p className="fab-eyebrow">About this example</p>
              <h3>{example.title}</h3>
              <p className="fab-summary">{example.summary}</p>
              <ol className="fab-story">
                {example.story.map((s, i) => (
                  <li key={i}>
                    {s.call !== undefined && onGoto ? (
                      <button className="fab-call" onClick={() => onGoto(s.call!)}>#{s.call}</button>
                    ) : (
                      <span className="fab-dot" aria-hidden />
                    )}
                    <span>{s.text}</span>
                  </li>
                ))}
              </ol>
            </section>
          )}
          <section className="fab-sec">
            <p className="fab-eyebrow">How to use {title}</p>
            <ol className="fab-steps">
              {steps.map((s, i) => (
                <li key={i}><span className="fab-n" aria-hidden>{i + 1}</span><span>{s}</span></li>
              ))}
            </ol>
            <a className="fab-more" href={`#/guide?s=${guide}`}>Read the full guide <span aria-hidden>→</span></a>
          </section>
        </div>
      )}
      {!open && nudge && <div className="fab-hint" aria-hidden>What happened here?</div>}
      <button className={`fab-btn ${nudge ? "nudge" : ""} ${open ? "on" : ""}`} onClick={() => { setOpen(!open); setNudge(false); }}
        aria-expanded={open} aria-controls={open ? id : undefined} aria-label={label} data-tip={example ? "About this example" : `How to use ${title}`}>
        {open ? "×" : "?"}
      </button>
    </div>
  );
}
