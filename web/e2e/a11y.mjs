// Accessibility check: axe-core on every page (desktop and phone). Fails on any serious or
// critical violation; lists moderate ones as warnings.
// Usage: node e2e/a11y.mjs [baseUrl]
import { chromium } from "playwright";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const axeSource = readFileSync(require.resolve("axe-core/axe.min.js"), "utf8");
const base = process.argv[2] ?? "http://localhost:4173/Khonsu/";
const PAGES = [
  ["landing", "", "Travel back to the moment"],
  ["guide", "#/guide", "How to use Khonsu"],
  ["downloads", "#/downloads", "Get Khonsu"],
  ["viewer", "#/viewer?src=support-agent.khn", "integrity verified"],
  ["play", "#/play", "Branch #0"],
  ["simulator", "#/simulator", "Recorded run of issue #42"],
  ["bench", "#/bench", "Against the alternatives"],
];

const browser = await chromium.launch();
const failures = [];
const warnings = new Map();
for (const [device, viewport] of [["desktop", { width: 1440, height: 900 }], ["phone", { width: 390, height: 844 }]]) {
  const page = await browser.newPage({ viewport, reducedMotion: "reduce" });  // settled layout, no mid-animation colors
  for (const [name, route, marker] of PAGES) {
    await page.goto(base + route);
    await page.getByText(marker).first().waitFor({ timeout: 30000 });
    await page.waitForTimeout(500);
    await page.addScriptTag({ content: axeSource });
    const res = await page.evaluate(async () =>
      // @ts-ignore: injected
      (await axe.run(document, { resultTypes: ["violations"] })).violations.map((v) => ({
        id: v.id, impact: v.impact, help: v.help, nodes: v.nodes.slice(0, 3).map((n) => n.target.join(" ")),
        count: v.nodes.length,
      })));
    for (const v of res) {
      const line = `${device}/${name}: [${v.impact}] ${v.id}: ${v.help} (${v.count}×) e.g. ${v.nodes.join(" | ")}`;
      if (v.impact === "serious" || v.impact === "critical") failures.push(line);
      else warnings.set(`${name}:${v.id}`, line);
    }
  }
  await page.close();
}
await browser.close();
if (warnings.size) console.log("warnings:\n" + [...warnings.values()].join("\n"));
if (failures.length) {
  console.error("\nA11Y FAILED:\n" + failures.join("\n"));
  process.exit(1);
}
console.log("A11Y OK");
