// Cross-browser, cross-device check: every page in Chromium, Firefox and WebKit (Safari's
// engine) at phone, tablet and desktop sizes. Fails on any page error, console error,
// horizontal overflow, or a page whose key content (including the WebAssembly worker)
// does not come up.
// Usage: node e2e/devices.mjs [baseUrl] [screenshotDir] [browsers=chromium,firefox,webkit]
import { chromium, firefox, webkit } from "playwright";
import { mkdirSync } from "node:fs";

const base = process.argv[2] ?? "http://localhost:4173/Khonsu/";
const shots = process.argv[3];
const only = (process.argv[4] ?? "chromium,firefox,webkit").split(",");
if (shots) mkdirSync(shots, { recursive: true });

const ENGINES = { chromium, firefox, webkit };
const DEVICES = [
  { name: "phone", viewport: { width: 390, height: 844 }, touch: true },
  { name: "tablet", viewport: { width: 820, height: 1180 }, touch: true },
  { name: "desktop", viewport: { width: 1440, height: 900 }, touch: false },
];
// Each page, the text that proves it rendered, and an optional interaction that
// proves the kernel worker (WebAssembly) runs in this engine.
const PAGES = [
  { route: "", marker: "Travel back to the moment" },
  { route: "#/viewer?src=coding-agent.khn", marker: "integrity verified" },
  { route: "#/play", marker: "Branch #0", then: async (p) => p.locator(".vm-out", { hasText: "opened" }).waitFor({ timeout: 30000 }) },
  { route: "#/simulator", marker: "Recorded run of issue #42" },
  { route: "#/bench", marker: "Against the alternatives" },
];

const failures = [];
for (const engine of only) {
  const browser = await ENGINES[engine].launch();
  for (const d of DEVICES) {
    const ctx = await browser.newContext({
      viewport: d.viewport,
      hasTouch: d.touch,
      isMobile: d.touch && engine !== "firefox",  // Firefox has no mobile emulation
      deviceScaleFactor: d.touch && engine !== "firefox" ? 2 : 1,
    });
    const page = await ctx.newPage();
    const errors = [];
    page.on("pageerror", (e) => errors.push(`pageerror: ${e.message}`));
    page.on("console", (m) => m.type() === "error" && errors.push(`console: ${m.text()}`));
    for (const pg of PAGES) {
      const label = `${engine}/${d.name}/${pg.route || "landing"}`;
      try {
        await page.goto(base + pg.route, { waitUntil: "load" });
        await page.getByText(pg.marker, { exact: false }).first().waitFor({ timeout: 30000 });
        if (pg.then) await pg.then(page);
        await page.waitForTimeout(300);
        // Touch devices: measure against the layout width (WebKit on Windows draws a classic
        // scrollbar in mobile emulation that real iOS does not have). Desktop: the client width.
        const measure = () => page.evaluate((layout) => {
          const W = Math.max(document.documentElement.clientWidth, layout);
          const over = document.documentElement.scrollWidth - W;
          if (over <= 1) return { over, who: [] };
          // Name the deepest elements that stick out, so a failure says what to fix.
          const who = [];
          for (const el of document.querySelectorAll("body *")) {
            const r = el.getBoundingClientRect();
            if (r.right <= W + 1 || r.width === 0) continue;
            if ([...el.children].some((c) => c.getBoundingClientRect().right > W + 1)) continue;
            const path = [];
            for (let n = el; n && n !== document.body && path.length < 4; n = n.parentElement)
              path.unshift(`${n.tagName.toLowerCase()}${n.className && typeof n.className === "string" ? "." + n.className.trim().split(/\s+/).join(".") : ""}`);
            who.push(`${path.join(" > ")} "${(el.textContent || "").trim().slice(0, 40)}" (right ${Math.round(r.right)})`);
          }
          return { over, who: who.slice(0, 6) };
        }, d.touch ? d.viewport.width : 0);
        let m = await measure();
        if (m.over > 1) {  // let late layout (canvases sizing to their container) settle, then judge
          await page.waitForTimeout(1500);
          m = await measure();
        }
        if (m.over > 1) failures.push(`${label}: horizontal overflow ${m.over}px from ${m.who.join(", ")}`);
        if (shots) await page.screenshot({ path: `${shots}/${engine}-${d.name}-${(pg.route || "landing").replace(/[^a-z]+/gi, "_")}.png` });
      } catch (e) {
        failures.push(`${label}: ${e.message.split("\n")[0]}`);
      }
      for (const e of errors.splice(0)) failures.push(`${label}: ${e}`);
    }
    await ctx.close();
    console.log(`  · ${engine} ${d.name}`);
  }
  await browser.close();
}

if (failures.length) {
  console.error("DEVICES FAILED:\n" + failures.join("\n"));
  process.exit(1);
}
console.log("DEVICES OK");
