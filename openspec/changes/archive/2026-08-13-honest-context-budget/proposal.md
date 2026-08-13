## Why

`graph_context` overshoots its stated token budget by ~6x while reporting a near-perfect fit, and fixing that reveals the default packing mode's measured advantage was the overshoot.

Measured on this repo with the default `gather=adaptive` and `budget: 3000`: the response was 73,590 bytes (~18,397 estimated tokens) while reporting `"tokens_used": 2999`. 104 of 159 entries (65%) carried no snippet, no line, and no location. Through MCP the response was refused outright as over the client token cap, so the agent received nothing at all from its primary context tool.

Cause: the knapsack weighs the capped source slice rather than the serialized entry (`slice_token_cost`). That is deliberate and sound for *ranking* — including JSON overhead flattens the weight spread and degenerates the knapsack toward greedy, which `research/2510.00446` established. The defect is that the same number is then reported as `tokens_used` and used as the budget test, so an entry that resolved no snippet weighed a few tokens while still shipping a ~200-byte mangled id and an absolute path. The greedy path is honest by construction and measures 1.2x.

A working fix exists and is preserved as `reference-implementation.patch`: keep the DP's ranking weight, then re-cost the selected set at its true serialized size and shed the lowest-value entry until it fits. Verified against a live daemon at budgets from 500 to 8000 — overshoot 6.1x becomes within-budget at every size, and every returned entry carries a snippet.

**The reason this is its own change rather than part of PR #17:** enforcing the budget erases the knapsack's advantage over greedy. On the committed frozen fixture (N=35, k=3):

| budget | knapsack, over-packing | knapsack, honest | greedy (always honest) | committed target |
| --- | --- | --- | --- | --- |
| 2000 | 0.590 | 0.445 | 0.462 | 0.591 |
| 4000 | 0.641 | 0.532 | 0.544 | 0.625 |
| 8000 | 0.660 | 0.624 | 0.620 | 0.666 |

At an equal, enforced budget greedy is at or above the knapsack at 2k and 4k and level at 8k. The two packers were never compared at the same real cost: greedy enforced its budget, the knapsack did not. The shipped win in `research/2510.00446` (+0.041/+0.032/+0.019) and the resulting default flip in `2026-06-18-default-adaptive-context-gather` both rest on that comparison.

`research/2510.00446/results.md` already recorded the warning shot — `cost_recheck.py` found the delta collapsing from +0.080 to +0.010 "under JSON-entry cost". With the budget actually enforced it reaches zero or negative at the budgets agents use.

Full measurement and method: `measurements.md` beside this file.

## What Changes

- Report the true serialized cost of the returned `included` array as `tokens_used`, measured identically for every packing mode, and never exceed `budget`.
- Keep the knapsack's slice-cost ranking weight unchanged; it is a selection heuristic, not a report.
- Mark an entry whose snippet could not be read with `snippet_omitted` in every packing mode, so a caller can tell a deliberate summary row from a failed read.
- Charge the focal entry first and never drop it, so a small budget still answers with the symbol asked about.
- **Decide, with measurement, whether `packing=knapsack` should remain the default** now that greedy matches or beats it at equal cost.
- **Decide whether a snippet-less entry counts as retrieved context.** Part of the recall drop is definitional: a label-only row with no line number counted toward recall while being close to useless to an agent. If it should not count, both packers need re-measuring under that definition.
- Re-pin the parity and recall gate baselines to whatever the honest packer produces, recording the reason and the commit the numbers came from.
- Non-goals: changing the gather strategy, the θ gate, or `kKnapsackContextDepth`; regenerating the eval fixture.

## Impact

- **Touches:** `src/engine/daemon_ops.cpp` (`pack_context` emit path), `tests/smoke/daemon_ops_test.cpp`, `tests/smoke/pack_context_parity_test.cpp`, `tests/smoke/retrieval_quality_test.cpp`.
- **This lowers two committed gates**, and that is the point of scoping it separately: it must be a deliberate, reviewed decision with the numbers in front of the reviewer, not a re-pin buried in a PR about extraction.
- **Agent-visible behavior changes**: a `context` call at a given budget returns fewer, richer entries. That is the intended correction — the previous payload was unusable through MCP — but it is a real change to what callers receive.
- Eval data is not modified. All numbers come from the committed frozen fixture.

## Capabilities

### Modified Capabilities

- `graph-daemon-client` — the `context` token budget contract, and possibly the default packing mode.
