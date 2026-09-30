// Downloads: the prebuilt CLI and both SDKs, straight from this site. Everything shown comes
// from downloads/downloads.json, which CI writes when it builds the package, so the version,
// sizes and checksums are always the real ones.
import { useEffect, useState } from "react";
import { fmtBytes } from "../lib/ui";

interface FileEntry { file: string; kind: "cli" | "python" | "js"; label: string; bytes: number; sha256: string }
interface Manifest { version: string; built: string; glibc: string; files: FileEntry[] }

const BASE = import.meta.env.BASE_URL;

function Copyable({ text, label }: { text: string; label?: string }) {
  const [done, setDone] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setDone(true);
      setTimeout(() => setDone(false), 1400);
    } catch {
      /* clipboard blocked: the text is selectable */
    }
  };
  return (
    <div className="dl-cmd">
      <code tabIndex={0}>{text}</code>
      <button className="btn small" onClick={copy} aria-label={label ?? "Copy command"}>{done ? "Copied" : "Copy"}</button>
    </div>
  );
}

export function Downloads() {
  const [m, setM] = useState<Manifest | null>(null);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    fetch(`${BASE}downloads/downloads.json`, { cache: "no-cache" })
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(String(r.status)))))
      .then(setM)
      .catch(() => setFailed(true));
  }, []);

  const site = `${location.origin}${BASE}`.replace(/\/$/, "");
  const url = (f: string) => `${site}/downloads/${f}`;
  const cli = m?.files.find((f) => f.kind === "cli");
  const whl = m?.files.find((f) => f.kind === "python");
  const tgz = m?.files.find((f) => f.kind === "js");

  return (
    <div className="dl">
      <header className="dl-hero">
        <p className="eyebrow enter">Download</p>
        <h1 className="display small-display enter" style={{ ["--d" as string]: "80ms" }}>Get Khonsu</h1>
        <p className="lede enter" style={{ ["--d" as string]: "160ms" }}>
          One command-line program and two small SDKs. Install it, put <code>khonsu record</code> in front of your agent's
          command, and every run becomes something you can replay, inspect and fork.
        </p>
        {m && <p className="dl-ver enter" style={{ ["--d" as string]: "220ms" }}>Version <b>{m.version}</b> · built {m.built}</p>}
      </header>

      {failed && (
        <div className="callout">Downloads are published with each release of the site. Please check back shortly.</div>
      )}

      {m && cli && (
        <>
          <section className="dl-card dl-main reveal in">
            <h2>Install in one line</h2>
            <p className="muted">Linux x86-64, or Windows inside WSL. It downloads the package, verifies its SHA-256 checksum, and installs <code>khonsu</code> to <code>~/.local/bin</code>.</p>
            <Copyable text={`curl -fsSL ${url("install.sh")} | sh`} label="Copy the install command" />
            <p className="muted small">Prefer to look first? <a href={url("install.sh")} target="_blank" rel="noreferrer">Read the installer</a>, it is a short shell script.</p>
          </section>

          <section className="dl-platforms">
            <article className="dl-card">
              <h3>Linux</h3>
              <p>x86-64, glibc {m.glibc} or newer: Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch, and most current distributions.</p>
              <span className="chip good">available</span>
            </article>
            <article className="dl-card">
              <h3>Windows</h3>
              <p>Run the same install command inside WSL. No WSL yet? Open PowerShell as administrator and run <code>wsl --install</code>, then restart.</p>
              <span className="chip good">via WSL</span>
            </article>
            <article className="dl-card">
              <h3>macOS</h3>
              <p>A native build is on the way. Until then, Khonsu runs in a Linux VM or container on your Mac.</p>
              <span className="chip">coming soon</span>
            </article>
          </section>

          <section className="dl-card">
            <h2>Files</h2>
            <div className="dl-files">
              {m.files.map((f) => (
                <div className="dl-file" key={f.file}>
                  <div className="dl-file-main">
                    <b>{f.label}</b>
                    <span className="mono muted">{f.file} · {fmtBytes(f.bytes)}</span>
                    <span className="mono dl-sha" title={f.sha256}>SHA-256 {f.sha256}</span>
                  </div>
                  <a className="btn primary" href={url(f.file)} download>Download</a>
                </div>
              ))}
            </div>
            <p className="muted small">
              Verify a manual download: put it next to <a href={url("SHA256SUMS")} download>SHA256SUMS</a> and run{" "}
              <code>sha256sum -c --ignore-missing SHA256SUMS</code>.
            </p>
          </section>

          <section className="dl-card">
            <h2>Tool SDKs</h2>
            <p className="muted">
              Optional. Wrap the tools your agent calls (sending email, writing files) so replays don't repeat their side
              effects. Both are included in the CLI package too.
            </p>
            {whl && (<><h3>Python</h3><Copyable text={`pip install ${url(whl.file)}`} label="Copy the pip command" /></>)}
            {tgz && (<><h3>JavaScript / TypeScript</h3><Copyable text={`npm install ${url(tgz.file)}`} label="Copy the npm command" /></>)}
          </section>

          <section className="dl-card">
            <h2>Then</h2>
            <Copyable text="khonsu record -- python agent.py" />
            <p className="muted">
              That's the whole setup: no code changes, and it works with any language or framework that talks to an
              OpenAI-compatible or Anthropic API. The <a href="#/guide?s=own">guide</a> walks through replay, fork, tests and
              sharing.
            </p>
          </section>
        </>
      )}
    </div>
  );
}
