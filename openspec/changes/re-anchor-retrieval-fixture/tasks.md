# Tasks

## 1. Regenerate the pair

- [x] 1.1 Build the engine at the merged HEAD (0cb8237) in a clean worktree; run the one-shot
      twice; `graph.json` byte-identical across runs (sha256
      ac55f4ec…c261c79 both). 1580 nodes / 3178 links.
- [x] 1.2 `python3 scripts/bootstrap_eval.py --root . --graph <out>/graph.json --out <tmp>` under
      the committed `.research-eval.toml`; 125 rows (75 symbol-granularity). No config edits.
- [x] 1.3 Relativize each node's `source_file` (strip the generation root); ids untouched
      (opaque, cross-referenced by queries.jsonl only).

## 2. Diagnose what the new fixture surfaced (did this before touching any pin)

- [x] 2.1 Both structural gate properties failed on the new pair: knapsack trailed greedy by up
      to 8.6 points (band 3), adaptive (then implying knapsack) trailed fixed-k2 greedy by
      2.6–4.3 points. Per-row probe through the real daemon path attributed it to the knapsack's
      cost-blind per-item value (research/packer-regression/results.md, reproducible harness).

## 3. Fix the packing (red first)

- [x] 3.1 daemon_ops_test regression coverage: default context self-describes
      gather=adaptive/packing=greedy; explicit knapsack keeps a large equally-relevant slice
      under a budget that fits either it or the confetti. Verified red against the pre-fix
      engine (exit 1), green after.
- [x] 3.2 Decouple `use_knapsack` from `adaptive`; adaptive keeps depth 3; `reach` accompanies
      adaptive under either fill.
- [x] 3.3 Knapsack value ×= √(slice cost).
- [x] 3.4 Greedy fill skips zero-overlap `same_file` candidates (the knapsack encoded this as
      zero value; greedy has no value gate, so the equivalent is the skip).

## 4. Re-anchor the gates

- [x] 4.1 Swap the fixture pair in; paths repository-relative (gate-asserted).
- [x] 4.2 Re-derive the adaptive revalidation block: non-inferiority vs fixed-k2 greedy plus
      candidate pool strictly below fixed-k3; the old "material gain" floor was measured on the
      sparse fixture and is not achievable by any config on the dense one.
- [x] 4.3 Re-pin, as transcriptions of gate output at root length 58: parity greedy
      0.407062/0.493927/0.530070/0.547631 and knapsack 0.398159/0.481693/0.519019/0.537489 at
      2000/4000/6000/8000; end-to-end 0.1804/0.2330/0.2795/0.2936.
- [x] 4.4 Update the fixture README: counts, generation sha, relativization step, root length.

## 5. Prove it

- [x] 5.1 Full `ctest --preset default` green; full default suite green 66/66 (39s at HEAD build). Sanitizers via CI (local ASan-built binaries hang
      at startup on this machine — recorded in add-rust-import-resolution).
- [x] 5.2 PR documents the regression numbers, the fix deltas, and old-vs-new pins (re-anchor
      provenance, not a win claim).
