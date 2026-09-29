// End-to-end smoke test against a served build (vite preview / GitHub Pages).
// Usage: node e2e/smoke.mjs [baseUrl] [screenshotDir]
import { chromium } from "playwright";
import { mkdirSync } from "node:fs";

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
const chapter = (title) => page.locator(".chapter", { hasText: title }).getByRole("button");

await page.goto(base);
await expectText("Recorded run of issue #42");
await page.waitForTimeout(900);
await shot("01-boot");

await chapter("Root cause").click();
await expectText("hallucinated API");
await shot("02-root-cause");

await chapter("Fork & fix").click();
await expectText("LLM calls replayed from the recording");
await page.waitForTimeout(600);
await shot("03-fork-fix");

await chapter("Speculate").click();
await expectText("Speculated 8 sampled branches");
await page.waitForTimeout(600);
await shot("04-speculate");

await chapter("Trust, but verify").click();
await expectText("divergence at step", 20000);
await shot("05-verify-tamper");

await chapter("Chaos").click();
await expectText("replayed bit-exactly");
await page.waitForTimeout(500);
await shot("06-chaos");

await page.goto(base + "#/bench");
await expectText("Fork latency vs state size");
await page.getByRole("button", { name: /Quick run/ }).click();
await expectText("live result from this browser", 60000);
await shot("07-bench");

// Mobile layout sanity: no horizontal overflow.
await page.setViewportSize({ width: 390, height: 844 });
await page.goto(base);
await expectText("Recorded run of issue #42");
const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
await shot("08-mobile");

await browser.close();
if (overflow > 1) errors.push(`horizontal overflow on mobile: ${overflow}px`);
if (errors.length) {
  console.error("E2E FAILED:\n" + errors.join("\n"));
  process.exit(1);
}
console.log("E2E OK");
