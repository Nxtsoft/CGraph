// Benchmark the graph.html viewer's client-side cost, without modifying the
// generated HTML. We wrap requestAnimationFrame/cancelAnimationFrame via an init
// script that runs before the page's own script, then treat the layout as
// "settled" once no animation frame has been scheduled for a quiet window.
//
// Reports, per input file: payload bytes, DOMContentLoaded time, time-to-settle
// (force-sim cool-down), and peak JS heap.
//
// Usage: node bench.mjs <graph.html> [labelN]
import { chromium } from "playwright";
import { statSync } from "node:fs";
import { pathToFileURL } from "node:url";

const file = process.argv[2];
const label = process.argv[3] || file;
if (!file) {
  console.error("usage: node bench.mjs <graph.html> [label]");
  process.exit(2);
}

const QUIET_MS = 400; // no rAF scheduled for this long => settled
const HARD_CAP_MS = 120000;

const initScript = `
  window.__raf = { pending: 0, lastSchedule: performance.now(), start: performance.now() };
  const _raf = window.requestAnimationFrame.bind(window);
  const _caf = window.cancelAnimationFrame.bind(window);
  window.requestAnimationFrame = (cb) => {
    window.__raf.pending++;
    window.__raf.lastSchedule = performance.now();
    return _raf((t) => { window.__raf.pending--; cb(t); });
  };
  window.cancelAnimationFrame = (h) => { if (window.__raf.pending > 0) window.__raf.pending--; return _caf(h); };
`;

const bytes = statSync(file).size;

const browser = await chromium.launch({ args: ["--no-sandbox", "--disable-gpu"] });
const page = await browser.newPage();
await page.addInitScript(initScript);

const t0 = Date.now();
await page.goto(pathToFileURL(file).href, { waitUntil: "domcontentloaded" });
const domMs = Date.now() - t0;

// Poll until the animation loop has been quiet for QUIET_MS (or hard cap).
let settleMs = null;
const deadline = Date.now() + HARD_CAP_MS;
while (Date.now() < deadline) {
  const st = await page.evaluate(() => {
    const r = window.__raf || { pending: 1, lastSchedule: performance.now(), start: performance.now() };
    return { pending: r.pending, quiet: performance.now() - r.lastSchedule, elapsed: performance.now() - r.start };
  });
  if (st.pending === 0 && st.quiet >= QUIET_MS) {
    settleMs = Math.round(st.elapsed - st.quiet);
    break;
  }
  await new Promise((r) => setTimeout(r, 50));
}

const heap = await page.evaluate(() =>
  performance.memory ? performance.memory.usedJSHeapSize : null,
);

await browser.close();

const row = {
  label,
  payload_mb: +(bytes / (1024 * 1024)).toFixed(2),
  dom_ms: domMs,
  settle_ms: settleMs === null ? ">120000 (capped)" : settleMs,
  peak_heap_mb: heap ? +(heap / (1024 * 1024)).toFixed(1) : null,
};
console.log(JSON.stringify(row));
