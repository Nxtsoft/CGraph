## Why

The committed retrieval fixture (`tests/fixtures/pack_context_parity/{graph.json,queries.jsonl}`)
arbitrates both retrieval gates and the spec-level same-file-expansion rule. It froze a graph of
1181 nodes / 1521 links with 38 rows mined through 2026-06-15. Since then the engine's output
changed materially — the C/C++ real call graph (#17), three enrichment-identity fixes
(#22/#26/#28), Rust extraction (#30) and Rust `use`-import resolution (#35) — and three archived
changes explicitly deferred regeneration ("the recall effect ... is reported as unmeasured ... it
wants its own change").

Regenerating the pair at 0cb8237 (1580 nodes / 3178 links, 75 symbol rows) did what a gate
regeneration is for: it surfaced a real regression the sparse fixture could not see. The default
context path (adaptive gather, which implied the knapsack fill) measured 2.6–5.9 recall points
BELOW plain fixed-k2 greedy, and the knapsack trailed greedy by up to 8.6 points (parity band: 3).
Root cause (per-row, through the real daemon path — `research/packer-regression`): the knapsack's
per-item value ignores slice cost, so on a dense graph its DP optimum packs many tiny
weakly-relevant entries (21 entries, median ~143 tokens) while shedding the 240–313-token symbols
the query is about. The fix and the re-anchor must land atomically: the gates can only be
re-pinned once, on post-fix behavior over the new fixture. This supersedes the change's original
"no packing changes" non-goal.

The observable contracts tests verify: the default context path never trails the plain fixed-k2
baseline; the knapsack keeps large equally-relevant slices; both gates measure on a fixture
regenerated at a recorded HEAD with transcribed baselines.

## What Changes

- **Decouple adaptive gather from the knapsack fill** (`daemon_ops.cpp`): gather and fill are
  independent; the default is adaptive gather + greedy fill; `packing="knapsack"` stays opt-in.
  Measured effect: default-path recall 0.434 → 0.493 @4000 (focal-injected), and end-to-end
  free-text recall roughly doubles at every budget (e.g. 0.0995 → 0.1804 @2000).
- **Scale knapsack item value by √(slice cost)**: equally-relevant entries compete on relevance,
  not on being small. Measured: knapsack 0.407 → 0.482 @4000; greedy/knapsack deltas land at
  0.009–0.012 across all budgets, inside the 0.03 parity band everywhere.
- **Keep the same-file lexical gate under both fills**: same-file admission packs only on lexical
  evidence — zero knapsack value (as before) and skipped by the greedy fill (new, since the
  default fill is now greedy). The adaptive `reach` summary now accompanies adaptive responses
  under either fill (it describes the gather stage).
- **Regenerate the fixture pair** at 0cb8237 by the documented procedure (two-run byte-identity,
  `bootstrap_eval.py` under the committed config, `source_file` relativization) and **re-pin both
  gates** to transcribed post-fix measurements in the same change; re-derive the adaptive
  revalidation block to non-inferiority + bounded pool (its old "material gain" floor was an
  artifact of the sparse fixture).
- Non-goals: tuning `.research-eval.toml` or bootstrap_eval grading; changing gather relevance
  gating or thresholds; adding an enrichment overlay to the fixture; keeping the old fixture.

## Impact

- `src/engine/daemon_ops.cpp` — fill decoupling, knapsack value scaling, greedy same-file lexical
  gate, reach summary under greedy.
- `tests/smoke/daemon_ops_test.cpp` — default self-description updated; new knapsack value-model
  regression block.
- `tests/smoke/pack_context_parity_test.cpp`, `tests/smoke/retrieval_quality_test.cpp` — re-pinned
  baselines; adaptive block re-derived.
- `tests/fixtures/pack_context_parity/{graph.json,queries.jsonl,README.md}` — regenerated pair +
  provenance.
- Capability: `graph-daemon-client`. Graph exports and graph.json parity are untouched (packing is
  a query-path concern).
