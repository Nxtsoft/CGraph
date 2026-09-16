# Fix: detection skips test-runner output trees (`playwright-report/`, `test-results/`, `blob-report/`)

## Why

Both new report views tripped over the same directory on the `frontend` Next.js app.
`playwright-report/` holds Playwright's HTML report, including seven minified trace-viewer bundles
(`trace/sw.bundle.js`, 5 lines, 95 kB; `trace/assets/defaultSettingsView-*.js`, 649 kB). The
extractor turns each bundle into hundreds of one-line functions calling one another, so:

- `report design` (#87) ranked nine of its top ten entry points by reach inside
  `playwright-report/trace/` (`yo` at reach 2,291, `processListReport` at 2,247); 237 of the
  1,349 entry points lived there.
- `report clones` (#85) found 46 of its 216 clone classes entirely inside it, including a
  nine-member class of identical one-line event handlers.

The directory is not in that repository's `.gitignore`, so the gitignore-aware walk did not
exclude it either. It is generated output, like `coverage/` and `dist/`, which the skip list
already names.

## What Changes

- `is_skipped_directory` gains `playwright-report`, `test-results` and `blob-report`: Playwright's
  default HTML-report, per-test-artifact and blob-report directories. Because the detector, the
  file watcher and the enrichment planner share this list, all three skip the trees.
- `path_ignore_test.cpp` asserts the three names skip and that the runner's source-side names
  (`tests`, `test-utils`, `playwright`) do not.
- The detection-exclusion requirement names test-runner output among the skipped tree kinds.

### Non-goals
- Other runners' output directories (Cypress `cypress/videos`, Jest `coverage/` is already listed).
  Add them when a graph shows them, with the measurement, as here.
- A general "minified file" heuristic. A one-line 649 kB file is unmistakable, but the fix that
  belongs at the source is the directory the runner writes to.

## Impact

- **Touches:** `src/engine/path_ignore.cpp`, `tests/smoke/path_ignore_test.cpp`.
- `graph.json` for repositories with these directories loses the generated nodes and edges; every
  other repository is byte-identical.
- Measured on `frontend` (same tree, same machine), before = origin/main at 892eb3e, after = this
  change: graph 17,439 -> 14,302 nodes; nodes under `playwright-report/` or `test-results/` 3,137
  -> 0; `report design` (no scope) 1,349 -> 1,071 entry points with the 237 bundle entries gone and
  the top six now `ProjectDetailPage`, `NextRoundPage`, `StagingCommitBridge`,
  `ProductDashboardPage`, `handleExportPdf`, `reconcilePendingProposal`; `report clones` 216 -> 170
  classes, the 46 bundle-only classes gone; one-shot build 10.1 s (extraction 4.0 s, layout 3.9 s).

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — detection also excludes test-runner output trees.
