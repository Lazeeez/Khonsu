// Performance check: frame rate and long tasks on every page with the CPU throttled 4x
// (roughly a mid-range phone), first contentful paint, and the landing page's JS weight.
// Usage: node e2e/perf.mjs [baseUrl] [screenshotDir]
import { chromium } from "playwright";
import { mkdirSync } from "node:fs";

const base = process.argv[2] ?? "http://localhost:4173/Khonsu/";
const shots = process.argv[3];
if (shots) mkdirSync(shots, { recursive: true });
const browser = await chromium.launch();
const failures = [];

async function measure(page, ms = 3000) {
  return page.evaluate((ms) => new Promise((done) => {
    const frames = [];
    let long = 0, longMs = 0;
    const po = new PerformanceObserver((l) => l.getEntries().forEach((e) => { long++; longMs += e.duration; }));
    try { po.observe({ entryTypes: ["longtask"] }); } catch {}
    const t0 = performance.now();
    const tick = (t) => {
      frames.push(t);
      if (t - t0 < ms) requestAnimationFrame(tick);
      else {
        po.disconnect();
        const gaps = frames.slice(1).map((f, i) => f - frames[i]);
        gaps.sort((a, b) => a - b);
        done({ fps: Math.round(((frames.length - 1) * 1000) / (frames.at(-1) - frames[0])),
               p95: Math.round(gaps[Math.floor(gaps.length * 0.95)] ?? 0), dropped: gaps.filter((g) => g > 50).length,
               long, longMs: Math.round(longMs) });
      }
    };
    requestAnimationFrame(tick);
  }), ms);
}

for (const [label, vp] of [["desktop", { width: 1440, height: 900 }], ["phone", { width: 390, height: 844 }]]) {
  const ctx = await browser.newContext({ viewport: vp, deviceScaleFactor: label === "phone" ? 2 : 1, isMobile: label === "phone", hasTouch: label === "phone" });
  const page = await ctx.newPage();
  const cdp = await ctx.newCDPSession(page);
  await page.goto(base, { waitUntil: "load" });
  await page.waitForTimeout(500);
  const { fcp, jsBytes } = await page.evaluate(() => ({
    fcp: performance.getEntriesByName("first-contentful-paint")[0]?.startTime ?? -1,
    // what the landing page itself loaded (idle prefetches of other pages start later)
    jsBytes: performance.getEntriesByType("resource")
      .filter((e) => e.initiatorType === "script" && e.startTime < 400)
      .reduce((n, e) => n + (e.encodedBodySize || e.transferSize || 0), 0),
  }));
  await page.waitForTimeout(3200);  // after the moonrise
  if (shots) await page.screenshot({ path: `${shots}/perf-${label}-landing.png` });
  if (label === "desktop" && shots) {
    await page.locator(".hero-dial svg").focus();
    await page.keyboard.press("End");  // full moon
    await page.waitForTimeout(1200);
    await page.locator(".hero-dial").screenshot({ path: `${shots}/perf-moon-full.png` });
    await page.keyboard.press("Home");
    await page.keyboard.press("ArrowRight");
    await page.keyboard.press("ArrowRight");
    await page.waitForTimeout(1200);
    await page.locator(".hero-dial").screenshot({ path: `${shots}/perf-moon-crescent.png` });
  }
  await cdp.send("Emulation.setCPUThrottlingRate", { rate: 4 });
  const results = { fcp: Math.round(fcp) };
  for (const [name, route, prep] of [
    ["landing", "", null],
    ["guide", "#/guide", null],
    ["viewer", "#/viewer?src=coding-agent.khn", "integrity verified"],
    ["play (VM running)", "#/play", "Branch #0"],
    ["simulator", "#/simulator", "Recorded run of issue #42"],
  ]) {
    await page.goto(base + route);
    if (prep) await page.getByText(prep).first().waitFor({ timeout: 60000 });
    await page.waitForTimeout(800);
    results[name] = await measure(page);
  }
  await cdp.send("Emulation.setCPUThrottlingRate", { rate: 1 });
  console.log(`\n${label} (CPU 4x slower): first contentful paint ${results.fcp} ms, landing JS ${Math.round(jsBytes / 1024)} KiB`);
  for (const [k, v] of Object.entries(results)) {
    if (k === "fcp") continue;
    console.log(`  ${k.padEnd(20)} ${String(v.fps).padStart(3)} fps · p95 frame ${v.p95} ms · ${v.dropped} frames >50 ms · ${v.long} long tasks (${v.longMs} ms)`);
    if (v.fps < 40) failures.push(`${label}/${k}: ${v.fps} fps`);
  }
  await ctx.close();
}
await browser.close();
if (failures.length) {
  console.error("\nPERF BELOW TARGET:\n" + failures.join("\n"));
  process.exit(1);
}
console.log("\nPERF OK");
