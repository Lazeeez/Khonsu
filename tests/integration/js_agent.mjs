// A Node agent for the TypeScript/JavaScript SDK suite (run_js.py): tools of every
// replay kind, then one model call whose prompt depends on the tools' results.
import { appendFileSync, writeFileSync } from "node:fs";
import { KhonsuDivergence, tool } from "../../sdk/js/src/index.js";

const clock = tool("clock", () => Date.now() + Math.random());                       // skip: replayed
const notify = tool("notify", (msg) => { appendFileSync(process.env.SIDE_EFFECTS, msg + "\n"); return "sent"; });
const writeFile = tool("write_file", (path, content) => { writeFileSync(path, content); return content.length; }, { replay: "rerun" });
const lint = tool("lint", (path) => `${path}: ok (pid ${process.pid}, ${Math.random()})`, { replay: "rerun" });
const explode = tool("explode", () => { throw new RangeError("boom"); });
const bigint = tool("bigint", () => 10n);                                             // not JSON

const started = await clock();
const wrote = await writeFile("answer.md", `# answer\nstarted ${started}\n`);
const report = await lint("answer.md");
let errName = "";
try { await explode(); } catch (e) { errName = `${e.name}: ${e.message}`; }
let badName = "";
try { await bigint(); } catch (e) { badName = e.name; }

const res = await fetch(`${process.env.OPENAI_BASE_URL}/chat/completions`, {
  method: "POST",
  headers: { "Content-Type": "application/json", Authorization: `Bearer ${process.env.OPENAI_API_KEY}` },
  body: JSON.stringify({ model: "mock-1", messages: [{ role: "user", content: `started ${started}; ${report}` }] }),
});
const answer = (await res.json()).choices[0].message.content;

let divergence = "";
try {
  await notify(process.env.NOTIFY_MESSAGE ?? "agent finished");
} catch (e) {
  divergence = e instanceof KhonsuDivergence ? "KhonsuDivergence" : e.name;
}
console.log(JSON.stringify({ started, wrote, report, errName, badName, answer, divergence }));
