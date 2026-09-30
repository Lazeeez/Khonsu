// The Guide: everything a first-time visitor needs, on the site itself. What Khonsu is,
// a click-by-click tour of each page, how to use it on your own agent, the concepts in
// plain words, the full CLI, and answers to the obvious questions.
import { useEffect, useRef, useState } from "react";
import { useReveal } from "../lib/motion";

const SECTIONS = [
  { id: "what", title: "What Khonsu is" },
  { id: "tour", title: "Take the tour" },
  { id: "own", title: "Use it on your agent" },
  { id: "tools", title: "Tools and side effects" },
  { id: "concepts", title: "Concepts" },
  { id: "cli", title: "Command reference" },
  { id: "faq", title: "Questions" },
];

function Code({ children }: { children: string }) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(children.replace(/\s+#.*$/gm, "").trim());
      setCopied(true);
      setTimeout(() => setCopied(false), 1400);
    } catch {
      /* clipboard blocked: the text is still selectable */
    }
  };
  return (
    <div className="g-code">
      <pre>{children}</pre>
      <button className="g-copy" onClick={copy} aria-label="Copy to clipboard">{copied ? "Copied" : "Copy"}</button>
    </div>
  );
}

function Step({ n, children }: { n: number; children: React.ReactNode }) {
  return (
    <li className="g-step">
      <span className="g-n" aria-hidden>{n}</span>
      <div>{children}</div>
    </li>
  );
}

const TOUR = [
  {
    id: "viewer", href: "#/viewer?src=coding-agent.khn", name: "Viewer", phase: 0.25,
    what: "Look inside a recorded run: every model call, every tool call, and the files as they were at each moment.",
    steps: [
      <>Open the <b>Coding agent</b> sample. It is a real recording of an agent editing a small repository.</>,
      <>Drag the moon around the <b>Moon Dial</b>, or use the arrow keys. Each mark on the orbit is one call; gold marks are model calls, turquoise marks are tools, red marks are failures.</>,
      <>Read the selected call on the right: the prompt, the model's answer, tool calls, tokens and timing.</>,
      <>Under <b>Workspace</b>, click a changed file to see exactly what that call did to it.</>,
      <>Press <b>Compare with a re-run</b>: Khonsu lines up two runs of the same task and shows the first moment they diverged.</>,
      <>Drop your own <code>.khn</code> recording, or an <b>OpenTelemetry GenAI</b> trace (JSON), anywhere on the page. It is parsed in your browser and never uploaded.</>,
    ],
  },
  {
    id: "play", href: "#/play", name: "Play", phase: 0.5,
    what: "Fork a program while it runs: the same idea as forking an agent, at the level of single instructions.",
    steps: [
      <>A program starts running by itself. Watch its output and the branch list.</>,
      <>Press <b>⑂ Fork</b> (or the <kbd>F</kbd> key) while it runs. A second branch appears that shares everything with the first.</>,
      <>Select the new branch, change a variable in the <b>Variables</b> list, and press <b>Run</b>: a "what if?" future.</>,
      <>Watch <b>Copy-on-write pages</b>: grey pages are still shared with the fork point; gold and turquoise are the only memory the fork had to copy.</>,
      <>Choose the <b>Gambler</b> preset and press <b>Run all</b> under Speculate: eight futures from the same instant, side by side.</>,
      <>Write your own program in the editor and press <b>Compile &amp; run</b>. The language is described under the editor.</>,
    ],
  },
  {
    id: "simulator", href: "#/simulator", name: "Simulator", phase: 0.75,
    what: "The engine underneath: a whole team of agents replayed, forked and verified in your browser.",
    steps: [
      <>Work through the six chapters at the top in order: <b>The incident</b>, <b>Root cause</b>, <b>Fork &amp; fix</b>, <b>Speculate</b>, <b>Trust, but verify</b>, <b>Chaos</b>. Each is one button.</>,
      <>The <b>Timeline multiverse</b> draws every timeline as a lane. Same color means the same world state.</>,
      <>Click any node to inspect that moment: agent memory, files, and the exact log frames behind it.</>,
      <>Use the scrubber and <b>⑂ Fork</b> or <b>✎ Inject</b> to branch from any step yourself.</>,
    ],
  },
  {
    id: "bench", href: "#/bench", name: "Benchmarks", phase: 1,
    what: "The evidence: Khonsu measured against the tools teams use today, and the engine timed on your own device.",
    steps: [
      <>Read <b>Against the alternatives</b>: a real agent on a local model, and head-to-head numbers against vcrpy and LangGraph.</>,
      <>Press <b>Quick run</b> to time the engine in your own browser.</>,
    ],
  },
];

function Phase({ f }: { f: number }) {
  // A tiny moon for the tour cards: the lit fraction marks the step in the journey.
  const r = 11, rx = r * Math.abs(2 * f - 1), sweep = f > 0.5 ? 1 : 0;
  return (
    <svg width="26" height="26" viewBox="0 0 26 26" aria-hidden className="g-phase">
      <circle cx="13" cy="13" r={r} fill="#0b1633" stroke="rgba(233,185,73,.35)" />
      <path d={`M13 2 A${r} ${r} 0 0 1 13 24 A${rx} ${r} 0 0 ${sweep} 13 2Z`} fill="#efe3c4" />
    </svg>
  );
}

export function Guide() {
  const root = useRef<HTMLElement>(null);
  const [active, setActive] = useState("what");
  useReveal(root);

  // #/guide?s=<section> deep links (the hash is taken by the router, so sections use a query).
  useEffect(() => {
    const s = new URLSearchParams(location.hash.split("?")[1] ?? "").get("s");
    if (s) requestAnimationFrame(() => document.getElementById(`g-${s}`)?.scrollIntoView({ block: "start" }));
  }, []);

  useEffect(() => {
    if (typeof IntersectionObserver === "undefined") return;
    const io = new IntersectionObserver(
      (es) => es.forEach((e) => e.isIntersecting && setActive(e.target.id.slice(2))),
      { rootMargin: "-30% 0px -60% 0px" },
    );
    SECTIONS.forEach((s) => {
      const el = document.getElementById(`g-${s.id}`);
      if (el) io.observe(el);
    });
    return () => io.disconnect();
  }, []);

  const go = (id: string) => document.getElementById(`g-${id}`)?.scrollIntoView({ behavior: "smooth", block: "start" });

  return (
    <main className="guide" ref={root}>
      <header className="g-hero">
        <p className="eyebrow enter">The guide</p>
        <h1 className="display small-display enter" style={{ ["--d" as string]: "80ms" }}>How to use Khonsu</h1>
        <p className="lede enter" style={{ ["--d" as string]: "160ms" }}>
          Khonsu records what an AI agent does, so you can replay it exactly, look at any moment, and change the future
          from there. This page explains the idea, walks you through every page of this site, and shows how to use it on
          your own agent.
        </p>
        <div className="g-flow enter" style={{ ["--d" as string]: "240ms" }} aria-label="Record, replay, fork">
          {[
            ["Record", "Run your agent through Khonsu once. Every call and file change is saved to one .khn file."],
            ["Replay", "Run it again from the file: same answers, no model, no cost, every time."],
            ["Fork", "Go back to any call, change something, and let only the new part go live."],
          ].map(([h, p], i) => (
            <div className="g-flow-step" key={h}>
              <Phase f={(i + 1) / 3} />
              <h3>{h}</h3>
              <p>{p}</p>
            </div>
          ))}
        </div>
      </header>

      <div className="g-layout">
        <nav className="g-toc" aria-label="On this page">
          {SECTIONS.map((s) => (
            <button key={s.id} className={active === s.id ? "on" : ""} onClick={() => go(s.id)}>{s.title}</button>
          ))}
        </nav>

        <div className="g-body">
          <section id="g-what" className="g-sec reveal">
            <h2>What Khonsu is</h2>
            <p>
              AI agents are hard to debug because they never do the same thing twice. The model samples a different answer on
              every run, so when an agent fails, running it again usually hides the bug. Tests that call a model are slow,
              expensive and flaky. And re-running an agent repeats its side effects: the email is sent again, the files are
              rewritten again.
            </p>
            <p>
              Khonsu sits between your agent and the model API and records everything: each request and response, each tool
              call, and a snapshot of your working directory before every call. From that one recording you can:
            </p>
            <ul className="g-list">
              <li><b>Replay</b> the run bit-for-bit, offline, as many times as you like, for free.</li>
              <li><b>Travel</b> to any call and see the prompt, the answer, and every file as it was at that moment.</li>
              <li><b>Fork</b> from any call: everything before it is replayed, everything after it runs for real.</li>
              <li><b>Diff</b> two runs and find the exact call and JSON field where they went different ways.</li>
              <li><b>Test</b> agents in CI with no model at all: a recording turns a flaky test into a deterministic one.</li>
            </ul>
            <p>
              It works with any language and any framework that talks to an OpenAI-compatible or Anthropic API (OpenAI,
              Anthropic, Ollama, vLLM and others), because it records at the HTTP boundary. Everything on this website runs in your browser; the
              recorder itself is a command-line tool.
            </p>
          </section>

          <section id="g-tour" className="g-sec reveal">
            <h2>Take the tour</h2>
            <p>Four pages, about five minutes. Each card says what to click and what to look for.</p>
            <div className="g-tour">
              {TOUR.map((t) => (
                <article key={t.id} id={`g-${t.id}`} className="g-card spot">
                  <div className="g-card-h">
                    <Phase f={t.phase} />
                    <h3>{t.name}</h3>
                    <a className="btn small primary" href={t.href}>Open {t.name} <span aria-hidden>→</span></a>
                  </div>
                  <p className="g-what">{t.what}</p>
                  <ol className="g-steps">
                    {t.steps.map((s, i) => <Step key={i} n={i + 1}>{s}</Step>)}
                  </ol>
                </article>
              ))}
            </div>
          </section>

          <section id="g-own" className="g-sec reveal">
            <h2>Use it on your own agent</h2>
            <p>
              Khonsu is a single command-line program. You put it in front of your agent's command; it starts a local proxy,
              points your agent at it through <code>OPENAI_BASE_URL</code>, and writes the recording. Your agent's code does
              not change.
            </p>
            <h3>1. Install</h3>
            <p>
              On Linux, or on Windows inside WSL, one command downloads Khonsu, verifies its checksum and puts it on your
              PATH. The <a href="#/downloads">Download</a> page has the files, checksums and SDKs.
            </p>
            <Code>{`curl -fsSL ${location.origin}${import.meta.env.BASE_URL}downloads/install.sh | sh`}</Code>
            <h3>2. Record a run</h3>
            <Code>{`khonsu record -- python agent.py
# ☾ khonsu recorded 7 calls (5 tools) · 9 workspace snapshots → .khonsu/runs/run-….khn`}</Code>
            <p>
              The model is whatever your agent already uses. For a free local model, point Khonsu at Ollama with{" "}
              <code>--upstream http://localhost:11434/v1</code>. API keys are passed through but never written to the recording.
            </p>
            <h3>3. Replay it</h3>
            <Code>{`khonsu replay run.khn -- python agent.py
# ☾ khonsu replayed 7/7 from recording · 0 live · 0 divergences · workspace identical`}</Code>
            <p>
              If your agent now sends something different (you changed a prompt, a tool returns new data), Khonsu stops and
              names the exact field that changed. Add <code>--on-miss live</code> to keep going: unchanged calls still come
              from the recording and only the changed ones reach the model.
            </p>
            <h3>4. Look inside</h3>
            <Code>{`khonsu show run.khn                 # every call: prompt, answer, tools, tokens, files changed
khonsu cat run.khn@4 src/app.py     # a file as it was just before call #4
khonsu checkout run.khn@start /tmp/before   # the whole workspace at a moment
khonsu verify run.khn               # integrity: checksums and the hash chain`}</Code>
            <p>Or drop the <code>.khn</code> file on the <a href="#/viewer">Viewer</a> page.</p>
            <h3>5. Fork from a call</h3>
            <Code>{`khonsu fork run.khn@4 -- python agent.py`}</Code>
            <p>
              Khonsu restores the files as they were at the start, replays calls 0–3 from the recording, and goes live from
              call #4. The result is saved as a new recording that remembers its parent.
            </p>
            <h3>6. Make agent tests deterministic</h3>
            <Code>{`import pytest

@pytest.mark.khonsu("recordings/refund_flow.khn")
def test_refund_flow():
    assert run_my_agent("I want a refund").action == "refund"`}</Code>
            <p>
              <code>pytest</code> replays the recording: offline, free, the same answer every time. Record or refresh it with{" "}
              <code>pytest --khonsu-record</code>. If your agent's requests change, the test fails and names the call and field.
            </p>
            <h3>7. Compare and share</h3>
            <Code>{`khonsu diff yesterday.khn today.khn          # where two runs went different ways
khonsu store add run.khn --name refund-bug    # keep it in a local store
khonsu sync http://team-store:7700 --token T  # share with a teammate (only new data moves)
khonsu annotate refund-bug@4 "hallucinated the API here"`}</Code>
          </section>

          <section id="g-tools" className="g-sec reveal">
            <h2>Tools and side effects</h2>
            <p>
              Model calls are recorded automatically. Tools (functions your agent runs, like sending email or writing files)
              live inside your program, so you mark them with one wrapper. Khonsu then records their results, and on replay
              decides whether to run them again.
            </p>
            <div className="g-two">
              <div>
                <h3>Python</h3>
                <Code>{`import khonsu

@khonsu.tool                   # replay: returns the recorded result
def send_email(to: str, body: str) -> str: ...

@khonsu.tool(replay="rerun")   # replay: runs again (writes the file)
def write_file(path: str, text: str) -> int: ...`}</Code>
              </div>
              <div>
                <h3>JavaScript / TypeScript</h3>
                <Code>{`import { tool } from "khonsu";

const sendEmail = tool("sendEmail", async (to, body) => { ... });

const writeFile = tool("writeFile", (path, text) => { ... },
  { replay: "rerun" });`}</Code>
              </div>
            </div>
            <table className="data g-table">
              <thead><tr><th>Mode</th><th>On record</th><th>On replay</th><th>Use for</th></tr></thead>
              <tbody>
                <tr><td>skip (default)</td><td>runs, result saved</td><td>not run; the saved result is returned</td><td>anything external: email, payments, APIs, the clock</td></tr>
                <tr><td>rerun</td><td>runs, result saved</td><td>runs again for its effect; the saved result is returned, and a difference is reported</td><td>writing files the agent reads back</td></tr>
              </tbody>
            </table>
          </section>

          <section id="g-concepts" className="g-sec reveal">
            <h2>Concepts</h2>
            <dl className="g-dl">
              <dt>Recording (<code>.khn</code>)</dt>
              <dd>One file holding a whole run: every request and response, tool results, and workspace snapshots. Append-only and checksummed, so a crash never loses what the agent already saw.</dd>
              <dt>Call</dt>
              <dd>One request to the model, or one tool call, numbered from #0. <code>run.khn@4</code> means "the moment just before call #4".</dd>
              <dt>Replay</dt>
              <dd>Running your agent again with answers served from the recording. Requests are matched by their content, so parallel calls replay correctly in any order.</dd>
              <dt>Divergence</dt>
              <dd>During a replay, a request that is not in the recording. Khonsu reports the first JSON field that differs, for example <code>messages[3].content</code>.</dd>
              <dt>Fork</dt>
              <dd>A new run that replays a recording up to a call and goes live from there. It is saved with a link to its parent.</dd>
              <dt>Workspace snapshot</dt>
              <dd>Your project folder, captured before every call. Unchanged files are stored once, so snapshots are small. <code>.gitignore</code> is respected, and secrets such as <code>.env</code> and keys are never captured.</dd>
              <dt>Moon Dial</dt>
              <dd>The Viewer's time control. The run is an orbit, each call a point on it, and the moon waxes as you travel from the first call to the last.</dd>
            </dl>
          </section>

          <section id="g-cli" className="g-sec reveal">
            <h2>Command reference</h2>
            <div className="g-scroll">
              <table className="data g-table">
                <thead><tr><th>Command</th><th>What it does</th></tr></thead>
                <tbody>
                  {[
                    ["khonsu record [-o FILE] [--upstream URL] [-w DIR | --no-workspace] -- CMD", "Run CMD and record every call. The workspace defaults to the current directory if it is a git checkout."],
                    ["khonsu replay FILE [--on-miss fail|live] [--realtime] -- CMD", "Run CMD against the recording: no model, no network. --on-miss live continues past changes."],
                    ["khonsu fork FILE@N [--dir DIR] -- CMD", "Restore the files, replay calls before #N, go live from #N."],
                    ["khonsu serve (--record FILE | --replay FILE)", "Run the proxy until Ctrl-C and print the environment to use."],
                    ["khonsu show FILE [--json]", "List every call, tool and workspace change."],
                    ["khonsu diff A B [--json]", "Align two runs call by call and show where they diverge."],
                    ["khonsu verify FILE", "Check every frame's checksum and the hash chain."],
                    ["khonsu files FILE@REF · cat FILE@REF PATH", "List the workspace, or print one file, at start, end or call N."],
                    ["khonsu checkout FILE@REF DIR [--force]", "Write out the whole workspace at a moment."],
                    ["khonsu store add | ls | get | rm", "Keep recordings in a deduplicated store."],
                    ["khonsu store serve [--port N] [--token T]", "Share a store over HTTP."],
                    ["khonsu sync REMOTE [--push | --pull]", "Exchange recordings and notes with another store."],
                    ["khonsu annotate NAME[@N] TEXT", "Attach a note to a run or a call; notes merge without conflicts."],
                  ].map(([c, d]) => (
                    <tr key={c}><td><code>{c}</code></td><td>{d}</td></tr>
                  ))}
                </tbody>
              </table>
            </div>
            <p className="muted small">The upstream model is --upstream, else $KHONSU_UPSTREAM, else your existing $OPENAI_BASE_URL, else OpenAI.</p>
          </section>

          <section id="g-faq" className="g-sec reveal">
            <h2>Questions</h2>
            <details className="g-faq" open>
              <summary>Does anything leave my machine?</summary>
              <p>No. The recorder runs locally and only talks to the model API your agent already uses. This website runs entirely in your browser; files you open here are never uploaded.</p>
            </details>
            <details className="g-faq">
              <summary>Are my API keys stored?</summary>
              <p>No. Authorization headers and API keys are forwarded to the model but written to the recording only as <code>&lt;redacted&gt;</code>. Files such as <code>.env</code>, <code>*.pem</code> and SSH keys are never snapshotted.</p>
            </details>
            <details className="g-faq">
              <summary>Do I need to change my agent's code?</summary>
              <p>No for model calls: Khonsu sets <code>OPENAI_BASE_URL</code> for the process it runs. Tools are optional: wrap the ones with side effects so replays don't repeat them.</p>
            </details>
            <details className="g-faq">
              <summary>Which frameworks and models work?</summary>
              <p>Anything that reaches its model over an OpenAI-compatible or Anthropic HTTP API: the OpenAI and Anthropic SDKs, LangChain and LangGraph, CrewAI, your own loop, in any language. Models can be hosted (OpenAI, Anthropic: use <code>--upstream https://api.anthropic.com/v1</code>) or local (Ollama, vLLM). Streaming responses are recorded chunk by chunk.</p>
            </details>
            <details className="g-faq">
              <summary>What happens if I change a prompt and replay?</summary>
              <p>Khonsu stops at the first changed request and tells you the call and the JSON field. With <code>--on-miss live</code> it serves everything unchanged from the recording and sends only the changed calls to the model, so you pay only for what changed.</p>
            </details>
            <details className="g-faq">
              <summary>What if the agent crashes, or I kill it?</summary>
              <p>Every response is written to disk before your agent can see it, so the recording always contains everything the agent observed, even after <code>kill -9</code>.</p>
            </details>
            <details className="g-faq">
              <summary>What are Play and the Simulator for?</summary>
              <p>They show the engine underneath Khonsu. The same data structure that lets a recording fork cheaply lets a running program fork in microseconds (Play), and lets a whole team of agents be replayed, forked and verified (Simulator).</p>
            </details>
          </section>
        </div>
      </div>
    </main>
  );
}
