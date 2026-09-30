// Renders the link-preview image (public/og.png, 1200x630) from the landing page's hero.
// Usage: node e2e/og-image.mjs [baseUrl]
import { chromium } from "playwright";

const base = process.argv[2] ?? "http://localhost:4173/Khonsu/";
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1200, height: 630 }, deviceScaleFactor: 1 });
await page.goto(base);
await page.getByText("Travel back to the moment").first().waitFor();
await page.addStyleTag({ content: ".topbar, .hero-note, .cta, .film { display: none !important; } .hero { min-height: 630px !important; padding-top: 10px; }" });
await page.waitForTimeout(3600);  // after the moonrise; the dial is mid-journey
await page.screenshot({ path: "public/og.png" });
await browser.close();
console.log("wrote public/og.png");
