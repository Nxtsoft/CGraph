# Enforcing the stated `context` budget erases the knapsack's advantage

**Status: measured, implemented, and deliberately parked.** The fix is correct in
isolation but it invalidates the basis for the shipped default, so it needs its own
change with its own experiment rather than riding along in a call-graph PR.

## The defect

`graph_context` with the default `gather=adaptive` overshoots its stated budget by
~6x while reporting a near-perfect fit. Measured on this repo before any fix, one
request with `budget: 3000`:

```
response      73,590 bytes  (~18,397 estimated tokens)
reported      "tokens_used": 2999
overshoot     6.1x
composition   104 of 159 entries (65%) carried no snippet, no line, no location
```

Through MCP the response was refused outright as over the client token cap, so the
agent received **nothing** from its primary context tool.

Cause: the knapsack weighs the capped source slice, not the serialized entry
(`daemon_ops.cpp`, `slice_token_cost`). That is deliberate and sound for *ranking* —
including JSON overhead flattens the weight spread and degenerates the knapsack
toward greedy, as `research/2510.00446` established. The defect is that the same
number is then reported as `tokens_used` and used as the budget test. An entry that
resolved no snippet weighed a few tokens while still shipping a ~200-byte mangled id
and an absolute path, so the packer believed it was nearly free and packed 159 of
them.

The greedy path is honest by construction and measured 1.2x.

## The fix (implemented, preserved here)

`reference-implementation.patch` in this directory is the working implementation, verified against a live daemon:

- Keep the DP's slice-cost ranking weight exactly as validated.
- After backtracking, render the selected entries, then measure the **array** and
  shed the lowest-value entry until it fits. Measuring the array rather than summing
  per-entry costs makes the ceiling exact — summing misses the array's own framing
  and overshot a 3000 budget by one token.
- Report the measured cost as `tokens_used`; count shed entries into `omitted`.
- Mark a failed snippet read with `snippet_omitted` in every packing mode, matching
  greedy.
- The focal entry is charged first and never dropped, so a small budget still
  answers with the symbol asked about.

Result, `gather=adaptive`, measured against a live daemon:

| budget | reported | measured `included` | within budget |
| --- | --- | --- | --- |
| 500 | 434 | 330 | yes |
| 1000 | 993 | 890 | yes |
| 3000 | 2950 | 2846 | yes |
| 4000 | 3999 | 3895 | yes |
| 8000 | 7995 | 7891 | yes |

Overshoot 6.1x → within budget at every size, and every returned entry now carries
a snippet.

## Why it is parked: the knapsack's win was the overshoot

`cgraph_pack_context_parity_test`, N=35 symbol rows, k=3, on the committed frozen
fixture. Command: `./build/release/tests/smoke/cgraph_pack_context_parity_test`.

| budget | knapsack, over-packing | knapsack, honest | greedy (always honest) | committed target |
| --- | --- | --- | --- | --- |
| 2000 | 0.590 | **0.445** | 0.462 | 0.591 |
| 4000 | 0.641 | **0.532** | 0.544 | 0.625 |
| 8000 | 0.660 | **0.624** | 0.620 | 0.666 |

Two conclusions:

1. **The knapsack's measured advantage over greedy was bought by shipping ~6x its
   stated budget.** At an equal, enforced budget greedy is *at or above* the
   knapsack at 2k and 4k, and level at 8k.
2. **The two packers were never compared at the same real cost.** Greedy enforced its
   budget honestly; knapsack did not. The shipped win in `research/2510.00446`
   (+0.041/+0.032/+0.019) and the resulting default flip to `gather=adaptive` /
   `packing=knapsack` (`2026-06-18-default-adaptive-context-gather`) rest on that
   comparison.

`research/2510.00446/results.md` already recorded the warning shot: `cost_recheck.py`
found the knapsack-over-greedy delta collapsing from +0.080 to +0.010 "under
JSON-entry cost". This measurement shows that with the budget actually enforced it
goes to zero or negative at the budgets agents actually use.

Part of the drop is also definitional: a label-only entry with no snippet and no line
number counted toward recall while being close to useless to an agent — the thing the
`snippet_omitted` marking exists to expose. Whether such a row should count as
retrieved context is a measurement question, not an implementation one.

## Which layer, and what a follow-up change owes

- **Layer:** context selection (packing), in `pack_context`.
- **Integration point:** `src/engine/daemon_ops.cpp`, the knapsack emit path — the
  patch sources here apply directly.
- **What the follow-up must decide:**
  1. Re-pin the parity targets to honest numbers, with the reason recorded.
  2. Re-examine whether `packing=knapsack` should remain the default, since greedy
     matches or beats it at equal cost.
  3. Decide whether a snippet-less entry counts as retrieved context, and if not,
     re-measure both packers under that definition.
- **Eval data changed: no.** All numbers above are from the committed frozen fixture
  `tests/fixtures/pack_context_parity/{graph.json,queries.jsonl}` (1181 nodes, 1521
  links, 38 rows / 35 symbol-granularity). Labels were not touched.
- **Graph measured on:** the frozen fixture above for recall; the live worktree graph
  (1434 nodes, 2843 edges) for the byte/token measurements. Absolute recall is only
  comparable within the same graph.

## Not to be confused with the call-graph change

`fix-cpp-call-resolution` (PR #17) does not touch `pack_context` and cannot move
these numbers: both retrieval gates read the frozen fixture, not a live build. That
is why this is a separate change.

---

## 2026-08-12 — implementation measurements and the two decisions

Implemented on top of the merged `complete-verified-snapshot` change (PR #19), which
had already made `tokens_used` an honest *report* (`emitted_entry_tokens`); this
change makes the budget an enforced *ceiling* in both packing modes and settles the
two owed decisions. All recall numbers below: committed frozen fixture, N=35
symbol rows, k=3, gather=fixed unless stated.

### The shed-order finding (review-sourced, confirmed)

The reference patch shed by ascending raw value, which systematically protects
cheap-looking high-value rows regardless of serialized cost. Shedding by value
DENSITY (value per serialized token) is what shipped. Four-arm decision table at
equal enforced budgets:

| budget | greedy | knapsack, density shed | knapsack, raw-value shed |
| --- | --- | --- | --- |
| 2000 | 0.442382 | 0.430765 | 0.413009 |
| 4000 | 0.515687 | 0.531519 | 0.518376 |
| **6000 (default)** | 0.574093 | **0.586637** | 0.582446 |
| 8000 | 0.608445 | 0.606290 | 0.611024 |

### Decision 1 — default packing: knapsack STAYS

Pre-registered rule: flip to greedy only if it beats the best knapsack arm by more
than kTol (0.03) at the shipped default budget (6000). Measured: knapsack-density
LEADS greedy at 6000 by +0.0125 (and at 4000 by +0.0158); greedy leads only at
2000, by +0.0116 — every delta inside the 0.03 band. The proposal's "greedy at or
above knapsack" projection was an artifact of the raw-value shed order. No default
flip; the welded `use_knapsack = packing=="knapsack" || adaptive` line is untouched.

### Decision 2 — what counts as retrieved context

Snippet-less rows are now split: `snippet_omitted` (has a source extent, the read
failed or was deliberately skipped to fit — includes greedy's brief-only rows,
which still carry file+line and are followable) vs `snippet_unavailable`
(structurally sourceless kinds: document/concept/media — followable pointers by
design). A row counts as retrieved context iff it is followable (snippet, or
location). Every entry in both committed fixtures is a code node with a location,
so this definition changes no number above; it is recorded so the next fixture
that mixes overlay nodes applies it. The genuinely useless row of the old packer
(no snippet, no line, no marker) can no longer be emitted at all.

### Why end-to-end recall pins dropped so hard

Live probe on this repo's daemon (1551 nodes, post-PR-19 binary, budget 2000):
the response measured ~12,700 tokens across 87 entries — 6.3x the stated budget —
at an average of ~553 bytes (~138 tokens) per entry (absolute path, sha256, id,
snippet). An honest 2000-token bundle holds roughly a dozen such entries, so the
end-to-end gate re-pins from 0.224/0.315/0.383 to 0.0745/0.1198/0.1928/0.2349 at
2k/4k/6k/8k. The old numbers were purchased with the overshoot. Callers that
relied on the old effective payload should raise `budget`.

### Gates re-shaped

- `pack_context_parity_test`: the one-sided `knapsack >= greedy` clause (satisfiable
  only by the overshoot) is now a symmetric parity band plus per-packer
  non-regression pins at 2k/4k/6k/8k. The adaptive revalidation block's
  `r_adp <= r_k3 + 0.02` cap assumed an unenforced budget (a superset could only
  help); with a ceiling a smaller better-ranked pool legitimately out-packs the
  superset, so the cap is removed and the 2k minimum gain re-pinned to the
  measured +0.0146 (floor 0.005).
- `retrieval_quality_test`: pins re-measured under the ceiling (provenance of the
  old pins was already orphaned by the d5030c1 fixture rewrite).
- Deliberate-regression probes verified both directions bite: disabling the shed
  fails the daemon_ops budget sweep; zeroing the ranking fails parity and the
  end-to-end gate.

### research/2510.00446 follow-up note

The +0.041/+0.032/+0.019 knapsack-over-greedy conclusion was measured at unequal
real cost (greedy enforced its budget, knapsack did not). At equal enforced
budgets the knapsack advantage survives only via density shed and only at
4k/6k (+0.016/+0.013), is a wash at 8k, and inverts at 2k (-0.012).
`cost_recheck.py`'s "collapses under JSON-entry cost" warning was correct in
direction. (research/ is gitignored; this section is the durable record.)
