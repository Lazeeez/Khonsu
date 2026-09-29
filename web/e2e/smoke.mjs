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

const shot = async (name) => shots && page.screenshot({ path: `${shots}/${name}.png`, fullPage: true });
const expectText = async (text, timeout = 15000) => page.getByText(text, { exact: false }).first().waitFor({ timeout });
const step = (name) => console.log(`  · ${name}`);

// ── landing ──────────────────────────────────────────────────────────────────────────────────
await page.goto(base);
await expectText("Travel back to the moment");
step("landing renders");
await page.waitForTimeout(600);
await shot("01-landing");

// ── viewer: open a recorded run from the landing CTA ──────────────────────────────────────────
await page.getByRole("link", { name: "Open a recorded run" }).click();
await expectText("integrity verified");
const dial = page.getByRole("slider", { name: /Moon Dial/ });
await dial.focus();
await page.keyboard.press("ArrowRight");        // → call #1: the first model call
await expectText("Add a greeting endpoint");    // its prompt, rendered from the .khn by the wasm kernel
step("viewer loads a .khn and renders call detail");
await page.keyboard.press("ArrowRight");        // → call #2 (write_file)
await expectText("this call changed 1 file");
await page.locator(".file.changed").first().click();
await page.locator(".difflines .add").first().waitFor();
step("dial navigates; workspace diff for the call renders");
await shot("02-viewer");

await page.getByRole("button", { name: "Compare with a re-run" }).click();
await expectText("First divergence");
step("compare: run diff finds the first divergence");
await shot("03-compare");

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
for (const [route, marker] of [["", "Travel back"], ["#/viewer?src=coding-agent.khn", "integrity verified"], ["#/play", "Branch #0"], ["#/bench", "Against the alternatives"]]) {
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
