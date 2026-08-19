# Tasks

## 1. Experiment first (research/focal-ranking, measured through the real path)

- [x] 1.1 Stage-1 proxy screen over the committed fixture: idf is the only ranking candidate
      that helps everywhere (hit@1 0.32→0.36, MRR 0.423→0.439); path-signal and length-norm
      hurt. Measured lexical ceilings: anchor-token 0.733, anchor-file 0.867.
- [x] 1.2 Stage-2 engine experiments via the committed e2e gate: idf@k5 +0.026..0.103;
      k sweep (1/2/3/5/10) → k=3 best at ≥4000; attribution run (k=3, old scorer) is a wash,
      proving idf causal. Full table in research/focal-ranking/results.md.

## 2. Implement (red first)

- [x] 2.1 daemon_ops_test: multi-seed test asserts focus == "d" (the rare-term match — idf
      ordering) and the alpha seeds' union via included. Red against the old scorer (focus
      was "a" by centrality tie), green after.
- [x] 2.2 `lexical_matches`: idf-weighted overlap, single O(nodes) pass building df + per-node
      term sets together.
- [x] 2.3 `kFocalSeedCount` 5 → 3 with the measured rationale in the comment.

## 3. Re-pin and prove

- [x] 3.1 e2e gate re-pinned to transcriptions: 0.2175/0.3403/0.3864/0.4245 (root length 58).
- [x] 3.2 Warmed context latency, 50 calls, before vs after on the same graph and query:
      adjacent quiet-condition runs measure AFTER at or below BEFORE (context median
      127.8ms vs 140.0ms; query-op 35.3ms vs 45.5ms). Same-binary medians spread 2.5x
      with machine load, so only adjacent runs are comparable; no regression.
- [x] 3.3 Full `ctest --preset default` green (66/66) before commit; PR #37 CI green on all
      eight jobs including both sanitizers. (Checkbox lagged the run at commit time;
      corrected at archive.)
