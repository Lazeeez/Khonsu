// Readable code in the viewer: line numbers and light syntax highlighting for JSON, Python,
// JavaScript/TypeScript and Markdown. A small single-pass tokenizer, no dependency: the
// viewer only needs recordings to read like code, not an editor.
import { memo, useMemo } from "react";

export type Lang = "json" | "py" | "js" | "md" | "text";

export function langOf(path: string | undefined): Lang {
  const ext = (path ?? "").split(".").pop()?.toLowerCase() ?? "";
  if (ext === "json" || ext === "jsonl") return "json";
  if (ext === "py") return "py";
  if (["js", "mjs", "cjs", "ts", "tsx", "jsx"].includes(ext)) return "js";
  if (ext === "md" || ext === "markdown") return "md";
  return "text";
}

const KW: Record<string, Set<string>> = {
  py: new Set("def class return import from as if elif else for while in not and or is None True False with try except finally raise lambda yield async await pass break continue global nonlocal assert del".split(" ")),
  js: new Set("const let var function return import from export default if else for while in of new class extends this null undefined true false async await try catch finally throw typeof instanceof switch case break continue".split(" ")),
};

type Tok = [cls: string, text: string];

function tokenizeLine(line: string, lang: Lang): Tok[] {
  if (lang === "text") return [["", line]];
  if (lang === "md") {
    if (/^\s*#{1,6}\s/.test(line)) return [["h", line]];
    if (/^\s*([-*+]|\d+\.)\s/.test(line)) {
      const m = line.match(/^(\s*(?:[-*+]|\d+\.)\s)(.*)$/)!;
      return [["p", m[1]], ...mdInline(m[2])];
    }
    return mdInline(line);
  }
  const out: Tok[] = [];
  const re = lang === "json"
    ? /("(?:[^"\\]|\\.)*")(\s*:)?|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b|([{}[\],:])/g
    : lang === "py"
      ? /(#.*$)|("""[\s\S]*?"""|'''[\s\S]*?'''|[rbfu]*"(?:[^"\\]|\\.)*"|[rbfu]*'(?:[^'\\]|\\.)*')|(\b\d+(?:\.\d+)?\b)|(@\w+)|(\b[A-Za-z_]\w*\b)/g
      : /(\/\/.*$)|(`(?:[^`\\]|\\.)*`|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')|(\b\d+(?:\.\d+)?\b)|(@\w+)|(\b[A-Za-z_$][\w$]*\b)/g;
  let last = 0;
  for (let m = re.exec(line); m; m = re.exec(line)) {
    if (m.index > last) out.push(["", line.slice(last, m.index)]);
    if (lang === "json") {
      if (m[1]) {
        out.push([m[2] ? "k" : "s", m[1]]);  // a string followed by ':' is a key
        if (m[2]) out.push(["", m[2]]);
      } else if (m[3]) out.push(["n", m[3]]);
      else if (m[4]) out.push(["b", m[4]]);
      else out.push(["p", m[5]]);
    } else if (m[1]) out.push(["c", m[1]]);
    else if (m[2]) out.push(["s", m[2]]);
    else if (m[3]) out.push(["n", m[3]]);
    else if (m[4]) out.push(["d", m[4]]);
    else {
      const w = m[5];
      const next = line.slice(re.lastIndex).trimStart();
      out.push([KW[lang].has(w) ? "kw" : next.startsWith("(") ? "f" : "", w]);
    }
    last = re.lastIndex;
    if (re.lastIndex === m.index) re.lastIndex++;  // never loop on an empty match
  }
  if (last < line.length) out.push(["", line.slice(last)]);
  return out;
}

function mdInline(s: string): Tok[] {
  const out: Tok[] = [];
  const re = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\[[^\]]+\]\([^)]+\))/g;
  let last = 0;
  for (let m = re.exec(s); m; m = re.exec(s)) {
    if (m.index > last) out.push(["", s.slice(last, m.index)]);
    out.push([m[1] ? "s" : m[2] ? "kw" : "f", m[0]]);
    last = re.lastIndex;
  }
  if (last < s.length) out.push(["", s.slice(last)]);
  return out;
}

export const CodeView = memo(function CodeView({ text, lang = "text", numbers = true, className = "" }: {
  text: string; lang?: Lang; numbers?: boolean; className?: string;
}) {
  const lines = useMemo(() => {
    const ls = text.replace(/\n$/, "").split("\n");
    return ls.length > 4000 ? ls.slice(0, 4000).concat([`… ${ls.length - 4000} more lines`]) : ls;
  }, [text]);
  return (
    <pre className={`codeview ${numbers ? "numbered" : ""} ${className}`} data-lang={lang}>
      {lines.map((l, i) => (
        <span className="cl" key={i}>
          {tokenizeLine(l, lang).map(([c, t], j) => (c ? <span key={j} className={`t-${c}`}>{t}</span> : t))}
          {"\n"}
        </span>
      ))}
    </pre>
  );
});

/** Pretty-printed, highlighted JSON for any value. */
export function JsonView({ value }: { value: unknown }) {
  const text = useMemo(() => (typeof value === "string" ? value : JSON.stringify(value, null, 2) ?? "null"), [value]);
  return <CodeView text={text} lang={typeof value === "string" ? "text" : "json"} numbers={text.includes("\n")} />;
}

/**
 * A tool call's arguments as named fields. Short values stay inline; multi-line strings
 * (file contents, prompts) render as code blocks, highlighted by the file name next to them.
 */
export function ArgsView({ args }: { args: unknown }) {
  if (!args || typeof args !== "object") return <JsonView value={args} />;
  const entries: [string, unknown][] = Array.isArray(args) ? args.map((v, i) => [`#${i}`, v]) : Object.entries(args as object);
  if (!entries.length) return <div className="muted small">(no arguments)</div>;
  const pathLike = entries.map(([, v]) => v).find((v) => typeof v === "string" && /\.[a-z0-9]{1,6}$/i.test(v)) as string | undefined;
  return (
    <div className="args">
      {entries.map(([k, v]) => {
        const block = (typeof v === "string" && v.includes("\n")) || (v !== null && typeof v === "object");
        return (
          <div key={k} className={`arg ${block ? "block" : ""}`}>
            <span className="arg-k">{k}</span>
            {typeof v === "string" && v.includes("\n") ? (
              <CodeView text={v} lang={langOf(pathLike)} />
            ) : block ? (
              <JsonView value={v} />
            ) : (
              <code className={`arg-v ${typeof v === "string" ? "t-s" : "t-n"}`}>{typeof v === "string" ? v : JSON.stringify(v)}</code>
            )}
          </div>
        );
      })}
    </div>
  );
}
