# Render benchmark for `graph.html`

Quantifies the client-side cost of the interactive viewer emitted by
`export_graph_html` (`src/engine/export_json.cpp`), so rendering optimizations can
be measured instead of asserted.

It measures, per graph size, without modifying the generated HTML:

- **payload** — `graph.html` size on disk (the embedded `graphData` dominates)
- **DOMContentLoaded** — parse/load time of the document
- **time-to-settle** — wall-clock until the force-directed layout stops animating
  (detected by wrapping `requestAnimationFrame`; settled = no frame scheduled for 400ms)
- **peak JS heap** — `performance.memory.usedJSHeapSize`

## Run

```sh
# build cgraph first (see repo README), then:
CGRAPH=/path/to/cgraph SIZES="1000 5000 10000" bash bench/run.sh
```

Writes `bench/baseline-report.md`. Requires Python 3, Node, and Playwright's
chromium (`bunx playwright install chromium`).

## Pieces

- `gen_tree.py` — deterministic synthetic Python project of ~N nodes, grouped into
  packages (communities) with cross-file calls/imports for realistic edge density.
- `bench.mjs` — Playwright driver; loads a `graph.html`, detects layout settle,
  reports one JSON row.
- `run.sh` — generates a tree per size, scans it with `cgraph`, runs the driver,
  collects a markdown table.

## Why it exists

The current viewer runs an O(N²) force simulation on the browser main thread on every
page open (no precomputed layout, no spatial index) and redraws every node/edge each
frame. This harness pins the baseline so the server-side-layout and render-culling
optimizations can show their gains.
