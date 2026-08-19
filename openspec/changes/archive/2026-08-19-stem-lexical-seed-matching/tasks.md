# Tasks

## 1. Experiment first (research/beyond-lexical, both stages recorded)

- [x] 1.1 Stage-1 proxy: stemming lifts anchor@seeds 0.480 → 0.533 and the lexical anchor
      ceiling 0.733 → 0.747; file-seed lever also screened.
- [x] 1.2 Stage-2 through the real e2e gate: stemming +0.014..+0.035 at every budget; the
      file seed REJECTED (0.3529 vs 0.3865 baseline at 6000 — pool dilution) and parked.

## 2. Implement (red first)

- [x] 2.1 daemon_ops_test: "packs budgets" resolves `budget_packing` (zero exact shared
      terms); "xylophone zebras" still resolves nothing. Red against the pre-change engine,
      green after.
- [x] 2.2 `stem_term`/`stem_terms` + stemmed query/label terms and stem-df in
      `lexical_matches`; everything else exact-match.

## 3. Re-pin and prove

- [x] 3.1 e2e gate re-pinned to transcriptions at the final tree state:
      0.2487/0.3629/0.3927/0.4425 (root length 58). Measurement is tree-state-sensitive
      (the gate slices live worktree bytes for snippets) -- both engines were re-measured
      against the identical final tree for the proposal's before/after table.
- [x] 3.2 Warmed context latency, 50 calls x 3 runs per side, adjacent: before medians
      57.6/59.2/58.9ms, after 63.1/60.9/67.3ms -> +7.1% on the median of medians, under the
      10% rule. The stem pass costs ~4ms per lexical call on this 1580-node graph.
- [x] 3.3 Full `ctest --preset default` green (66/66); sanitizers via CI (local ASan hang
      recorded in add-rust-import-resolution).
