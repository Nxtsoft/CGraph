# Preview what a proposed change would break, and keep the answer as obligations

## Why

Today an agent learns what its edit broke only after writing it, from a failing build or a reviewer. CGraph can already answer "what depends on this" (`impact`, `path`), and `change_context` already turns a unified diff into symbol changes and their dependents. It works only on a change that already exists on disk, though: it needs a materialized `target_root` and runs a full `run_one_shot` build of both roots (`src/engine/change_context.cpp:371-372`).

The research report (area 3, `~/.agents/artifacts/2026-09-25/cgraph-context-engine-research/report.md`) found no engine that lets an agent apply a proposed diff to a branched graph *before* writing. It also found none that keeps the answer as obligations that survive a context reset: "update these callers, rerun these tests". Blastline computes the test obligation today, but only from a merged diff.

## What Changes

Two capabilities, shipped in order, each gated.

1. **`preview` (MCP `graph_preview`).** The agent passes a unified diff as text. The daemon checks it against the live files and the served snapshot. It builds a code-only base and a branch from the same warm per-file cache, re-extracting only the touched indexed files from their proposed contents, and runs full dedup on both. It returns the same shape as `change_context`: symbol changes, dependents on the base and the branch, and optional `path` checks. The branch is never published, and no file under the project root is written except, with `record: true`, new sidecars under `cgraph-out/memory/`. It runs on the serve loop through an injected handler, and refuses with a typed error while a build holds the index or before the index is hydrated.
2. **Obligation ledger (MCP `graph_obligations`, `graph_obligation_resolve`).** With `record: true`, each impacted consumer and test becomes an obligation stored as a memory sidecar, like a checkpoint. The engine derives what it can prove mechanically, with a fixed precedence: the obligation is `gone`, `proposed`, `diverged` (the landed change is not the one previewed), `revisited` or `open`, or `pending` while the watcher catches up. Only the host can mark an obligation `satisfied` or `waived`. A resolution reads `stale` once the consumer changes after it.

**Phase 0, the gate, before any engine code.** Replay merged multi-commit CGraph PRs via `refs/pull/N/head`. For each, compute the impact of the PR's first commit and measure how often it pointed at the files that later commits had to change. Compare against same-directory and git co-change baselines of the same size. The protocol is fixed in `design.md` (depth 3, union impact set, at least 15 PRs). Phase 1 starts only if the preview beats both baselines with a CI95 that excludes zero. Phase 2, the ledger, starts only after the preview has shipped and been used, with the owner's agreement.

## Contract that tests will verify

- A preview of a diff that deletes an exported function lists the function's callers as dependents, and it writes no file and publishes no snapshot. The served snapshot's content root is unchanged afterwards.
- A diff is rejected with a typed error, and nothing partial is returned, when its old side does not match the live file or an indexed file changed after the snapshot extracted it. It is also rejected when it touches the root `.gitignore`, `tsconfig.json` or `jsconfig.json`, exceeds the size or file-count cap, or arrives while a build is running or the index is not hydrated.
- A file the graph does not index, such as `CMakeLists.txt`, is checked against its live bytes and reported as `unassessed`. It is never a reason to reject.
- A preview's symbol changes and dependents equal `change_context`'s evidence builder run on the same file set with the diff applied to a materialized copy. Both are built as `rebuild_graph` followed by full dedup. This is the parity test, and it runs after an incremental update, with a semantic drop and a checkpoint present.
- A preview re-extracts only the indexed files the diff touches. Every other file's extraction comes from the warm cache.
- A recorded obligation survives a daemon restart and a rebuild, and reads each state in the design's precedence table as the code changes: `proposed`, `diverged`, `open`, `revisited`, `gone`, `pending`, and `satisfied`, `waived` or `stale` after a resolve.
- Obligation nodes are inert to ranking and never appear in `graph.json`, like checkpoints.

## Non-goals

- Running tests, or judging whether a revisited consumer is correct. The engine reports mechanical facts, and the host asserts satisfaction.
- Semantic invariants ("keep this invariant"): they are free text in a checkpoint body, and the engine does not check them.
- Workspace (multi-repo) previews. The design keeps them possible, since the branch rebuild is per member, but they are out of scope.
- Any model or LLM logic in the binary.

## Impact

- New shared unit `unified_diff`, holding the parser now private to `change_context.cpp` plus a new `apply_patch`. The existing `validate_patch` only compares against a target read from disk; it never produces text.
- New `change_evidence` unit: `change_context`'s response builder (today the `add_side` lambda) takes a source provider, so it works from disk or from memory. `change_context` moves onto it with no behaviour change, which its existing tests guard.
- `rebuild_graph` (private in `incremental_update.cpp`) becomes callable on an overlay of the index, with an optional stats out-parameter.
- `extract_detected_source` shares one body with `extract_detected_file`, including the 8 MiB cap.
- `is_test_module` moves out of `report.cpp`'s anonymous namespace.
- A new injected `preview_handler`, next to `update_handler`, because the index is owned by the daemon's run loop.
- New daemon ops `preview`, `obligations` and `obligation_resolve`. They are added to `DaemonOp` and to `kSubstantiveOps`, so they reach the durable op-stats ledger, additively as `remember`/`recall` did. `kProtocolVersion` is unchanged. A workspace root returns `workspace_op_unsupported`.
- MCP gains three tools. The host skill and `docs/` gain a section each.
