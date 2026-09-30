// "How to use this page": three to five steps at the top of each tool page, open on the
// first visit, collapsible after. Remembered per page in this browser (best effort).
import { useState } from "react";

const key = (id: string) => `khonsu.help.${id}`;

function readOpen(id: string): boolean {
  try {
    return localStorage.getItem(key(id)) !== "closed";
  } catch {
    return true;
  }
}

export function PageHelp({ id, title, steps }: { id: string; title: string; steps: React.ReactNode[] }) {
  const [open, setOpen] = useState(() => readOpen(id));
  const toggle = () => {
    const next = !open;
    setOpen(next);
    try {
      localStorage.setItem(key(id), next ? "open" : "closed");
    } catch {
      /* storage blocked: the panel just won't be remembered */
    }
  };
  return (
    <section className={`page-help ${open ? "open" : ""}`} aria-label={`How to use ${title}`}>
      <button className="ph-head" onClick={toggle} aria-expanded={open}>
        <span className="ph-q" aria-hidden>?</span>
        <span className="ph-title">How to use {title}</span>
        <span className="ph-hint">{open ? "Hide" : "Show the steps"}</span>
      </button>
      <div className="ph-body" hidden={!open}>
        <ol>
          {steps.map((s, i) => (
            <li key={i}><span className="ph-n" aria-hidden>{i + 1}</span><span>{s}</span></li>
          ))}
        </ol>
        <a className="ph-more" href={`#/guide?s=${id}`}>Read the full guide for this page <span aria-hidden>→</span></a>
      </div>
    </section>
  );
}
