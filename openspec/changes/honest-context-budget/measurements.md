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
