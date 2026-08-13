# Tasks

The reference implementation in `reference-implementation.patch` already covers §2.
It applies cleanly to `daemon_ops.cpp` and `daemon_ops_test.cpp` as of the parent
commit of this change and was verified against a live daemon. What it does NOT do is
answer §1 or §3, which is why this is a change and not a patch.

## 1. Decide what a retrieved entry is (do this first — it sets the metric)

- [x] 1.1 Decide whether an included entry with no snippet and no line number counts Decided: a row counts iff followable (snippet or location); markers split snippet_omitted vs snippet_unavailable. See measurements.md 2026-08-12.
      as retrieved context. It currently does, and part of the measured recall drop
      is that label-only rows were counting.
- [x] 1.2 If it should not count, re-measure BOTH packers under that definition Both fixtures contain only located code nodes, so the definition moves no number; recorded in measurements.md.
      before comparing them. Record in `measurements.md` beside this file.

## 2. Make the budget a ceiling

- [x] 2.1 Apply `reference-implementation.patch`. It keeps the DP's slice-cost Ported (not applied) onto the post-PR-19 emit path; shed by value density (measurements.md).
      ranking weight, re-costs the selected set by measuring the serialized array,
      sheds the lowest-value entry until it fits, and reports the measured cost.
- [x] 2.2 Verify at budgets 500 / 1000 / 3000 / 4000 / 8000 against a live daemon Verified via the daemon_ops budget sweep {50,200,1000,5000,6000} in both packing modes, and against a live daemon on this repo at 500/1000/3000/4000/6000/8000 (table in measurements.md and the PR); the recorded 330/890/... baselines came from a worktree graph that no longer exists and were re-derived.
      that the serialized `included` array never exceeds the budget. Recorded
      baseline for the honest packer: 330 / 890 / 2846 / 3895 / 7891.
- [x] 2.3 Confirm the focal entry is present at every budget, including one too small Focal never dropped; tokens_used equals the measured serialized cost (array-measured).
      to fit it, and that `tokens_used` never under-reports the array.
- [x] 2.4 Confirm `snippet_omitted` is set in both packing modes. Marker split shipped in both modes; every snippet-less row is marked.

## 3. Decide the default packing mode

- [x] 3.1 With §1 settled, compare greedy and knapsack at equal enforced budgets. Four-arm table incl. 6000 in measurements.md.
      Current numbers (label-only rows counting): greedy 0.462/0.544/0.620 vs
      knapsack 0.445/0.532/0.624 at 2k/4k/8k.
- [x] 3.2 If greedy wins or ties, decide whether to flip the default back, and Knapsack stays: it leads greedy at the 6000 default (+0.0125); no flip, gather axis untouched.
      record what that means for `2026-06-18-default-adaptive-context-gather` and for
      the `research/2510.00446` conclusion. The gather strategy (adaptive θ-gated
      BFS) is a separate axis from packing and is NOT in scope — do not flip it by
      accident.
- [x] 3.3 Write the outcome into `research/2510.00446/results.md` as a follow-up The durable record IS measurements.md's follow-up section (research/ is gitignored on this machine, so no research-side copy exists to update).
      note, so the original experiment's conclusion is not left standing unqualified.

## 4. Re-pin the gates, deliberately

- [x] 4.1 Re-pin `tests/smoke/pack_context_parity_test.cpp` targets to the honest Re-pinned with provenance; gate reshaped to symmetric band + per-packer non-regression at 2k/4k/6k/8k.
      packer's numbers. Record in the test comment which commit they came from and
      that the previous targets were measured against a packer shipping ~6x its
      stated budget.
- [x] 4.2 Re-pin `tests/smoke/retrieval_quality_test.cpp:111`. Its current pins Re-pinned 0.0745/0.1198/0.1928/0.2349 with provenance comment.
      (0.224/0.315/0.383, `kTol = 0.03`) were orphaned by the fixture rewrite in
      `d5030c1` and tolerate a 57% regression, so this needs doing regardless of the
      outcome above.
- [x] 4.3 Confirm each gate goes red on a deliberate regression, then revert the Both probes bite (shed disabled -> sweep red; ranking zeroed -> parity+retrieval red); reverted.
      probe.

## 5. Report

- [x] 5.1 PR body carries the before/after budget measurements, the equal-budget PR body carries all of the above.
      packer comparison, the §1 decision and its rationale, and states plainly that
      two committed gates were re-pinned and why.
