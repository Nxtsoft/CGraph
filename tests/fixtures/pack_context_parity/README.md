# Retrieval fixture (parity + end-to-end gates)

Committed fixture pair backing two smoke gates:

- `cgraph_pack_context_parity_test` — gather/packing parity with the offline harness
  (focal id injected per row, isolating the packing stage).
- `cgraph_retrieval_quality_test` — end-to-end grade-2 recall through the default
  `context` path (free-text query only; focal resolution on the measured path).

## Contents

- `graph.json` — deterministic code-only export of this repo at 0cb8237 (1580 nodes,
  3178 links; built from a clean worktree, so no `research/` or build-output nodes).
  All baseline numbers in both gates were measured on exactly this graph; absolute
  recall is only comparable within it.
- `queries.jsonl` — 125 git-mined eval rows (75 symbol-granularity), graded 2 for
  directly-changed symbols and 1 for graph neighbors. Verbatim snapshot of the
  `scripts/bootstrap_eval.py` output at 0cb8237; labels are query-derived, never
  label-derived.

## Regenerating

1. Build the engine in a clean checkout (no untracked inputs) and run the one-shot
   twice; assert the two `graph.json` outputs are byte-identical:
   `cgraph --root <repo> --out <tmpA>` / `--out <tmpB>`.
2. Rewrite each node's `source_file` to be repository-relative (strip the generation
   root prefix). Node ids keep their generation-root derivation — they are opaque and
   only cross-referenced against `queries.jsonl`.
3. `python3 scripts/bootstrap_eval.py --root <repo> --graph <tmpA>/graph.json --out <dir>`
   (config from the committed `.research-eval.toml`; no grading edits).
4. Copy the pair here.

Regenerating changes both gates' measured baselines: re-measure and re-pin the baseline
constants in `pack_context_parity_test.cpp` and `retrieval_quality_test.cpp` in the same
change, record the new node/link and row counts here, and record the root length the
pins were measured at (entry costs include the absolute source path). Current pins:
root length 58, at 0cb8237.
