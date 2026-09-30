// End-to-end smoke test against a served build (vite preview / GitHub Pages).
// Usage: node e2e/smoke.mjs [baseUrl] [screenshotDir]
import { chromium } from "playwright";
import { mkdirSync, readFileSync } from "node:fs";

const base = process.argv[2] ?? "http://localhost:4173/Khonsu/";
const shots = process.argv[3];
if (shots) mkdirSync(shots, { recursive: true });

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1600, height: 1000 }, deviceScaleFactor: 1 });
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
page.on("console", (m) => m.type() === "error" && errors.push(m.text()));

// Scroll through first so scroll-revealed sections are on the full-page capture.
const shot = async (name) => {
  if (!shots) return;
  await page.evaluate(async () => {
    for (let y = 0; y < document.body.scrollHeight; y += innerHeight / 2) { scrollTo(0, y); await new Promise((r) => setTimeout(r, 60)); }
    scrollTo(0, 0);
  });
  await page.waitForTimeout(1200);
  await page.screenshot({ path: `${shots}/${name}.png`, fullPage: true });
};
const expectText = async (text, timeout = 15000) => page.getByText(text, { exact: false }).first().waitFor({ timeout });
const step = (name) => console.log(`  · ${name}`);

// ── landing ──────────────────────────────────────────────────────────────────────────────────
await page.goto(base);
await expectText("Travel back to the moment");
await expectText("The same agent, the same task, three times.");
step("landing renders, with the study evidence");
await page.waitForTimeout(600);
await shot("01-landing");

// ── guide: the nav link, a tour card that deep-links into its page ──────────────────────────
await page.getByRole("link", { name: "Guide", exact: true }).click();
await expectText("How to use Khonsu");
await expectText("Take the tour");
await page.locator("#g-play").getByRole("link", { name: /Open Play/ }).waitFor();
step("guide renders with the tour and deep links");
await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Download", exact: true }).click();
await expectText("Install in one line");
const installCmd = await page.locator(".dl-main .dl-cmd code").innerText();
if (!/curl -fsSL .*\/downloads\/install\.sh \| sh/.test(installCmd)) errors.push(`install command looks wrong: ${installCmd}`);
for (const href of await page.locator(".dl-file a").evaluateAll((as) => as.map((a) => a.href))) {
  const r = await page.request.get(href);
  if (!r.ok()) errors.push(`download ${href}: HTTP ${r.status()}`);
}
step("downloads page: install command and every file is served");
await shot("01b-guide");
await page.goto(base);

// ── viewer: open a recorded run from the landing CTA ──────────────────────────────────────────
await page.getByRole("link", { name: "Open a recorded run" }).click();
await expectText("integrity verified");
const dial = page.getByRole("slider", { name: /Moon Dial/ });
await dial.focus();
await expectText("cracked base");                // the customer's message, rendered from the .khn by the wasm kernel
await page.keyboard.press("ArrowRight");        // → call #1: lookup_order
await page.locator(".arg", { hasText: "A1042" }).first().waitFor();
step("viewer loads a .khn and renders call detail");
await shot("02-viewer");

// the coding example: a file created at one call, changed at a later one
await page.getByRole("button", { name: /^Examples/ }).click();
await page.getByRole("menuitem", { name: /Coding agent/ }).click();
await page.getByRole("heading", { name: /Coding agent: failing test/ }).waitFor();  // loaded: selection resets to #0 first
await dial.focus();
await page.keyboard.press("Home");
for (let i = 0; i < 7; i++) await page.keyboard.press("ArrowRight");  // → call #7: write_file changes solution.py
await expectText("this call changed 1 file");
await page.locator(".file.changed").first().click();
await page.locator(".difflines .add").first().waitFor();
step("dial navigates; workspace diff for the call renders");

await page.getByRole("button", { name: "Compare with a re-run" }).click();
await expectText("First divergence");
step("compare: run diff finds the first divergence");
await shot("03-compare");

// Examples menu, the example's story behind the "?", and readable tool arguments.
await page.getByRole("button", { name: "Close compare" }).click();
await page.getByRole("button", { name: /^Examples/ }).click();
await page.getByRole("menuitem", { name: /Fork: a new future/ }).click();
await expectText("fork of");
await page.getByRole("button", { name: /What happened in this example/ }).click();
await expectText("About this example");
await page.locator(".fab-panel").getByRole("button", { name: "#6" }).click();
if ((await page.getByRole("slider", { name: /Moon Dial/ }).getAttribute("aria-valuenow")) !== "6") errors.push("story: #6 did not select call 6");
await page.keyboard.press("Escape");
await page.locator(".fab-panel").waitFor({ state: "detached" });
await page.getByRole("slider", { name: /Moon Dial/ }).focus();
await page.keyboard.press("ArrowRight");  // call #7: write_file(solution.py), a new implementation
await page.locator(".arg .codeview .cl", { hasText: "def to_roman" }).first().waitFor();
step("examples menu, example story with call links, formatted tool arguments");

// Drop-in import of an OpenTelemetry GenAI trace (no .khn needed).
const otel = readFileSync(new URL("./fixtures/otel-genai.json", import.meta.url));
await page.locator('input[type="file"]').first().setInputFiles({ name: "trace.json", mimeType: "application/json", buffer: otel });
await expectText("opentelemetry");
await expectText("What is the capital of Egypt?");
step("OpenTelemetry GenAI trace imported");

// ── simulator (the deterministic kernel) ──────────────────────────────────────────────────────
await page.goto(base + "#/simulator");
await expectText("Recorded run of issue #42");
const chapter = (title) => page.locator(".chapter", { hasText: title }).getByRole("button");
await chapter("Fork & fix").click();
await expectText("LLM calls replayed from the recording");
await chapter("Trust, but verify").click();
await expectText("divergence at step", 20000);
step("simulator: fork + verify/tamper");
await page.waitForTimeout(400);
await shot("04-simulator");

// ── play: fork a running program ──────────────────────────────────────────────────────────────
await page.goto(base + "#/play");
await expectText("Branch #0");
await page.locator(".vm-out", { hasText: "32768" }).waitFor({ timeout: 30000 });
await page.getByRole("button", { name: "⑂ Fork" }).click();
await expectText("Branch #1");
await expectText("forked from");
step("play: forked the ledger while it ran");
await page.getByRole("button", { name: "Run", exact: true }).click();  // the child; the parent keeps running
await page.getByRole("radio", { name: "max" }).click();
const finalOut = async (id) => {
  await page.getByRole("option", { name: new RegExp(`^#${id}\\b`) }).click();
  await expectText(`Branch #${id}`);
  await page.locator(".play-inspect .chip.status-halted").waitFor({ timeout: 60000 });
  return (await page.locator(".vm-out").innerText()).trim().split("\n").slice(-2).join(" ");
};
const [p0, p1] = [await finalOut(0), await finalOut(1)];
if (p0 !== p1 || !p0.includes("richest")) errors.push(`fork diverged without an edit: "${p0}" vs "${p1}"`);
await expectText("this branch copied");
step("play: parent and child finish identically; page map shows sharing");
await shot("05-play");
await page.getByRole("combobox", { name: "Preset program" }).selectOption({ label: "Gambler" });
await expectText("A gambler bets");
await page.locator(".play-inspect .vm-out").waitFor();
await page.getByRole("button", { name: "Run all" }).click();
await expectText("futures ran in", 30000);
step("play: speculative branches");
await page.getByRole("textbox", { name: "Lune source" }).fill("let x = 1\nprint y\n");
await page.getByRole("button", { name: "Compile & run" }).click();
await expectText("unknown variable 'y'");
step("play: compile errors point at the line");

// ── benchmarks ────────────────────────────────────────────────────────────────────────────────
await page.goto(base + "#/bench");
await expectText("Fork latency vs state size");
await expectText("replays identical");
await page.getByRole("button", { name: /Quick run/ }).click();
await expectText("live result from this browser", 60000);
step("benchmarks run in the browser");
await shot("06-bench");

// ── mobile: no horizontal overflow on the main pages ──────────────────────────────────────────
await page.setViewportSize({ width: 390, height: 844 });
for (const [route, marker] of [["", "Travel back"], ["#/viewer?src=support-agent.khn", "integrity verified"], ["#/play", "Branch #0"], ["#/bench", "Against the alternatives"]]) {
  await page.goto(base + route);
  await expectText(marker);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  if (overflow > 1) errors.push(`horizontal overflow on mobile (${route || "landing"}): ${overflow}px`);
}
await shot("07-mobile-viewer");

await browser.close();
if (errors.length) {
  console.error("E2E FAILED:\n" + errors.join("\n"));
  process.exit(1);
}
console.log("E2E OK");
