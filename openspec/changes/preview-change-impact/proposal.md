# Preview what a proposed change would break, and keep the answer as obligations

## Why

Today an agent learns what its edit broke only after writing it, from a failing build or a reviewer. CGraph can already answer "what depends on this" (`impact`, `path`), and `change_context` already turns a unified diff into symbol changes and their dependents. It works only on a change that already exists on disk, though: it needs a materialized `target_root` and runs a full `run_one_shot` build of both roots (`src/engine/change_context.cpp:371-372`).

The research report (area 3, `~/.agents/artifacts/2026-09-25/cgraph-context-engine-research/report.md`) found no engine that lets an agent apply a proposed diff to a branched graph *before* writing. It also found none that keeps the answer as obligations that survive a context reset: "update these callers, rerun these tests". Blastline computes the test obligation today, but only from a merged diff.

## What Changes

Two capabilities, shipped in order, each gated.

1. **`preview` (MCP `graph_preview`).** The agent passes a unified diff as text. The daemon checks it against the live files and the served snapshot. It then rebuilds a private branch graph from the warm per-file cache, re-extracting only the touched files from their proposed contents. It returns the same shape as `change_context`: symbol changes, dependents on the base and the branch, and optional `path` checks. The branch is never published and no file is written.
2. **Obligation ledger (MCP `graph_obligations`, `graph_obligation_resolve`).** With `record: true`, each impacted consumer and test becomes an obligation stored as a memory sidecar, like a checkpoint. The engine derives what it can prove mechanically: the change is still only proposed, the obligation is open, the consumer has been revisited since, or it is gone. Only the host can mark an obligation satisfied or waived, with a note.

**Phase 0, the gate, before any engine code.** Replay merged multi-commit CGraph PRs. For each, compute the impact of the PR's first commit and measure how often it pointed at the files that later commits had to change. Compare against same-directory and git co-change baselines of the same size. Phase 1 starts only if the preview beats both with a CI95 that excludes zero. The measurement runs with the existing `change_context`, because materialized roots give exactly what an in-memory preview would compute.

## Contract that tests will verify

- A preview of a diff that deletes an exported function lists the function's callers as dependents, and it writes no file and publishes no snapshot. The served snapshot's content root is unchanged afterwards.
- A diff whose old side does not match the live file, or whose file changed after the snapshot extracted it, is rejected. Nothing partial is returned.
- A preview's symbol changes and dependents equal `change_context` run on the same base plus the same diff with the target materialized on disk. This is the parity test.
- A preview re-extracts only the files the diff touches. Every other file's extraction comes from the warm cache.
- A recorded obligation survives a daemon restart and a rebuild, reads `proposed` until the diff lands, reads `open` once it does, `revisited` after the consumer's own lines change, and `gone` after the consumer is deleted. Only an explicit resolve makes it `satisfied` or `waived`.
- Obligation nodes are inert to ranking and never appear in `graph.json`, like checkpoints.

## Non-goals

- Running tests, or judging whether a revisited consumer is correct. The engine reports mechanical facts, and the host asserts satisfaction.
- Semantic invariants ("keep this invariant"): they are free text in a checkpoint body, and the engine does not check them.
- Workspace (multi-repo) previews. The design keeps them possible, since the branch rebuild is per member, but they are out of scope.
- Any model or LLM logic in the binary.

## Impact

- New shared unit `unified_diff` holding the parser and the patch application now private to `change_context.cpp`. `change_context` moves onto it with no behaviour change, which its existing tests guard.
- `rebuild_graph` (private in `incremental_update.cpp`) becomes callable on an overlay of the index.
- New daemon ops `preview`, `obligations` and `obligation_resolve`. `DaemonOp` and the op-stats ledger grow additively, as they did for `remember`/`recall`. `kProtocolVersion` is unchanged.
- MCP gains three tools. The host skill and `docs/` gain a section each.
