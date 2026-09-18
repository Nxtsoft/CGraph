# Make node identifiers repo-relative instead of absolute-path-derived

## Why

A node id is derived from the **absolute** path of its source file, so the machine that built
the graph is baked into every id.

Measured on the committed parity fixture `tests/fixtures/pack_context_parity/graph.json`
(1,580 nodes):

| | |
|---|---|
| Total characters across all node ids | 183,941 |
| Longest common prefix (the machine-local root) | 65 chars |
| Characters that are just that prefix | **102,700 — 55.8% of all id characters** |

The prefix in that committed fixture is
`users_taylorgagne_tools_cgraph_agents_worktrees_fixture_reanchor_` — a git worktree that no
longer exists on any machine.

**`source_file` is already relative.** Every one of those 1,580 nodes carries
`source_file: "bench/bench.mjs"`; **zero** carry an absolute `source_file`. The id is the only
field still holding machine-local state, and it is literally `<absolute-root>_<the same
relative path>`:

```
id         : users_taylorgagne_tools_cgraph_agents_worktrees_fixture_reanchor_bench_bench_mjs
source_file: bench/bench.mjs
```

**This already costs measurable retrieval quality**, and the repo says so in a test constant.
`tests/smoke/pack_context_parity_test.cpp:186-189`:

> Environment note: entry costs include the absolute source path, so recall moves with checkout
> depth; these pins were measured at root length 58 (a deep worktree), shorter real-world roots
> only add margin to the floors.

The retrieval gate's own floors are calibrated to a **filesystem path length**. A context entry
spends budget on a path prefix that carries no information about the code, so a deeper checkout
retrieves less at the same token budget.

Three consequences follow:

- **Identical source, different graphs.** Two checkouts of the same commit produce different node
  ids, so ids cannot be compared, cached, or referenced across machines or across worktrees.
- **Budget spent on nothing.** Over half of every id's characters are a constant prefix that no
  consumer needs.
- **Evidence is not portable.** Any artifact that records ids — a benchmark result, an
  investigation, a PR comment — is valid only on the machine that produced it.

## What Changes

- Feed `make_id` the **repo-relative** source path rather than the absolute one. The absolute
  path enters at a single site: `file_extraction.cpp:63` sets
  `ExtractionContext::source_file` from `file.path.generic_string()`, and
  `extractor.cpp:475` calls `make_id(ctx.source_file)`.
- `make_id` itself is **not touched**. The `ID normalization parity` requirement constrains the
  *transformation* — Unicode normalization, word-character handling, underscore collapse, case
  folding — not which string is passed to it.
- Regenerate committed goldens and fixtures whose ids carry a dead worktree path.
- Delete the checkout-depth environment note in the packing-parity gate once recall no longer
  varies with root length.

### Non-goals

- Changing `make_id`'s normalization behavior.
- Changing `source_file`, which is already relative.
- Repo-scoped ids (`repo:<name>`), which remain an explicit non-goal of the multi-repo track.
- Any change to edge semantics, dedup identity rules, or the content-root hash.

## The parity question this proposal must settle first

Graphify parity is a hard contract: "extraction fragment shape, ID normalization, and
`graph.json` node-link output must stay compatible."

`make_id`'s behavior is unchanged, so normalization parity is intact by construction. The open
question is narrower and load-bearing:

> **Does Graphify's `_make_id` receive an absolute or a repo-relative path?**

- If Graphify passes a **relative** path, then cgraph is the one that diverged, and this change
  restores parity rather than breaking it.
- If Graphify passes an **absolute** path, then relative ids diverge from Graphify's node-link
  output, and this becomes a deliberate, documented divergence — or the proposal is withdrawn.

**This cannot be answered from this repository.** It requires a real Graphify export of a known
fixture. Task 1 settles it, and no implementation task starts until it is settled. A proposal
that guessed here would be proposing to break a hard contract on a hunch.

## Impact

**Every persisted artifact keyed by node id dangles on the first rebuild after this lands:**

- Semantic cache dependency records resolve by `dependency.node_id`
  (`semantic_cache.cpp:335-338`); a missed lookup is recorded as
  `"dependency node missing: " + dependency.node_id` and invalidates the fragment.
- Memory sidecar fragments store `concerns` edges by node id (`daemon_ops.cpp:1949-1951`) and are
  re-overlaid after every graph rebuild.
- The persisted fast-load extraction index is keyed by the same ids.

None of these is a correctness hazard — each already handles a missing dependency by invalidating
and requeuing — but the first rebuild after this change will invalidate broadly, and that cost
must be stated up front rather than discovered.

**Also affected:** committed goldens and parity fixtures, the `graph.json` node-link export, and
the packing-parity floors (which should improve, and should stop depending on checkout depth).
