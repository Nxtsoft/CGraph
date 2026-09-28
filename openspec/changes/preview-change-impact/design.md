# Design: preview-change-impact

Revised after non-author design review. The review's findings B1–B7 and N1–N4 are addressed inline and cited where they changed the design.

## Test strategy first

| Behaviour | How it is tested |
|---|---|
| Preview equals change_context on a code-only base | `tests/smoke/preview_test.cpp`. The fixture daemon has had at least one incremental update, one semantic drop, and one checkpoint sidecar (B3: otherwise drift cannot show). The preview's `changes` and impacted ids must equal `change_context` run on the same base plus the diff applied to a temp copy. Both sides are built as `rebuild_graph` followed by full `semantic_dedup`, with defaults per `pipeline.cpp:101`. |
| The served graph is untouched | Snapshot pointer and `content_root` unchanged. No file is created or modified under the project root except, with `record: true`, new sidecars under `cgraph-out/memory/` (N2: this makes the check deterministic). |
| Rejections | Each case returns a typed error and no partial result: old side mismatch; an indexed file edited after extraction, where the edit is placed outside the hunk (N3); a path outside the root; the diff touches the root `.gitignore`, `tsconfig.json` or `jsconfig.json` (B3); the index is not hydrated (B2); a full build is in progress (B1); over the diff-size or file-count cap (N2). |
| Undetected files pass through | A diff touching `CMakeLists.txt` plus one `.cpp` file: the CMake file's old side is checked against its live bytes, it is not re-extracted, and it is reported under `unassessed` (B4). |
| Only touched indexed files re-extract | `files_reextracted` equals the touched indexed files, and the rest are cache hits. |
| Added files obey detection | An added gitignored file, dependency-directory file, Unknown-language file, or a file over 8 MiB is reported as skipped, never extracted (B3). |
| Obligation states | `tests/smoke/obligations_test.cpp`, with real files, covering every row of the state table below, including `diverged`, added and deleted files, and a consumer inside a file the diff touches. |
| Inertness | Obligation nodes never appear in `query`, `context`, `impact` or `path` results. A `record: true` preview never creates an obligation about an obligation (B6). |
| Mutation check | Without the overlay re-extraction, parity fails. Without the freshness gate, the out-of-hunk-edit test fails. Without the hydration guard, the fast-load test fails. |
| Real flow and latency | Exact commands in `tasks.md` (B7). |

## Where the preview runs (B1, B2)

The daemon serves every request inline on one serve-loop thread (`daemon_server.cpp:48-52`). The file index is a local in the daemon's run function, guarded by `graph_mutex` (`:322`, `:424`), not by `writer_mutex`. Requests reach index-owning code only through injected handlers, following the `update_handler` pattern (`daemon_ops.hpp:70-76`).

- **Access.** `preview` is a new injected `preview_handler`. It takes `graph_mutex` with `try_to_lock`, and if a build holds it, returns the typed error `building` rather than wedging the accept loop (`:848`). It reads the index through a const-ref overlay with no copy, which is safe because nothing else runs on this thread meanwhile.
- **Hydration.** After a fast-load restart the index holds no extractions (`:618`, `:730`). A preview then returns the typed error `index_not_hydrated`, telling the caller to run `graph_update`. The alternative, hydrating inside the request, would stall the serve loop for a full build. `index_hydrated` is already tracked (`:620`).
- **Cost, stated honestly.** A preview blocks other requests, the watcher poll and drop polling for its duration, exactly as `update` does. Task 3.4 measures it.

## Building base and branch (B3, B4)

```
diff text ──► parse (unified_diff) ──► caps: diff ≤ 1 MiB, ≤ 50 files (N2)
          ──► reject if touching root .gitignore / tsconfig.json / jsconfig.json
          ──► per touched path:
                indexed      → old side must match live bytes AND the snapshot's source_hashes (freshness gate)
                not indexed  → old side must match live bytes; pass through, report `unassessed`
                added        → detection rules (language, gitignore, dependency dir, 8 MiB cap), or skipped
          ──► apply_patch → new contents in memory (NEW code: validate_patch only compares; it never produces text)
          ──► base   = semantic_dedup(rebuild_graph(index))              code-only; no drops, no memory
              branch = semantic_dedup(rebuild_graph(index ⊕ overlay))    overlay = re-extracted touched files
          ──► impact on base and branch via the extracted response builder
```

- **Why a code-only base.** The served snapshot also carries semantic drops and memory sidecars (`:583-587`), and `trace_impact` has no memory or enrichment filter. Obligations would then breed obligations (B6), and parity with `run_one_shot` would be impossible. Both sides are built from one index view with full dedup, so they differ only by the diff.
- **Extraction from memory.** `extract_detected_source` shares one body with `extract_detected_file`, including the 8 MiB cap, which today lives inside `read_file` (`file_extraction.cpp:18`).
- **Response builder with a source provider (B5).** `add_side` is a lambda inside `change_context()`, and it reads from disk (`:455-461`, `:523-525`, `:546-547`). It moves into `change_evidence.{hpp,cpp}` taking a `SourceProvider(side, path)`. The disk provider serves `change_context` unchanged, and the in-memory provider serves the branch. `uncertainty.call_resolution` needs pipeline stats, so `rebuild_graph` gains an optional stats out-parameter.
- **Parity is defined against that base.** The preview equals `change_context`, provided `change_context` also builds with `rebuild_graph` plus dedup over the same file set. The parity test runs `change_context`'s own evidence builder on the materialized target through the disk provider, so the two ops share every line past graph construction.

## Obligation ledger (B6)

An obligation is a memory node `memory:obligation:<stamp>` stored as a sidecar under `cgraph-out/memory/`. Sidecars are written atomically (temp file plus rename), because a torn file is silently skipped on overlay (`:520`) (N2). `Properties` is string to string (`types.hpp:14`), so structured fields are stored JSON-encoded:

- `concerns` edge to the consumer, with `touch`. It is anchored only for base-side consumers in files the diff does not touch (`span_sha256` requires disk bytes to match the snapshot, `daemon_ops.cpp:164-167`).
- `branch_only: true` for consumers that exist only on the branch. They are stored by id and resolved again once the diff lands, never read as `gone` while the change is still proposed.
- `in_diff: true` for consumers inside a touched file. Their anchor is the branch span, compared once the file reaches its new hash.
- `change`: for each diff file, `{base: hash|"absent", new: hash|"absent"}`. The sentinels cover added and deleted files.
- `resolution`: `{status, note, anchor}`, where `anchor` is the consumer's span hash at resolution time. `stale` needs it.

### States, computed at read time, first matching row wins

For each diff file, the live snapshot hash classifies it as `base`, `new` or `other`.

| order | state | condition |
|---|---|---|
| 1 | `gone` | the consumer no longer resolves, and it was not `branch_only` while the change is still `proposed` |
| 2 | `satisfied` / `waived` | a resolution exists and the consumer's span still matches the resolution anchor |
| 3 | `stale` | a resolution exists and the span no longer matches its anchor |
| 4 | `proposed` | every diff file is `base` |
| 5 | `diverged` | some diff file is `other`, or a mix of `base` and `new`: the landed change is not the previewed one |
| 6 | `revisited` | every diff file is `new`, and the consumer's span differs from its anchor |
| 7 | `open` | every diff file is `new`, and the consumer's span matches its anchor |
| – | `pending` | a consumer or diff file is ahead of the snapshot (the watcher has not caught up), so its state is not yet known |

"Landed, then edited again" reads `diverged`. That is honest: the file is no longer the previewed result.

### Volume and idempotence (N2)

A record of the same diff sha256 against the same base content root is a no-op that returns the existing ids. All obligations from one preview share one sidecar, so each overlay reads one file per recorded preview, not one per obligation.

### Surfaces

- `graph_preview {diff, max_depth (1–5, default 3), budget, paths?, record?}`. Typed errors: `building`, `index_not_hydrated`, `not_caught_up`, `mismatch`, `too_large`, `config_touched`. In a workspace root it returns `workspace_op_unsupported`, via the existing fallback (`workspace.cpp:691-699`).
- `graph_obligations {state?, class?, limit?}` and `graph_obligation_resolve {id, status, note}`.
- New ops are added to `DaemonOp` and to `kSubstantiveOps` (`operation_stats.cpp:210-212`), so they reach the durable op-stats ledger.
- New sources: `unified_diff.cpp`, `change_evidence.cpp`, `preview.cpp`, `obligations.cpp`, each with its 1:1 test (N4). `daemon_ops.cpp` is already 2486 lines. `is_test_module` moves out of `report.cpp`'s anonymous namespace to classify `test` obligations.

## Alternatives considered

- **Materialize a temp tree and call `change_context`.** Seconds per call, and it writes a tree, so it is never pre-edit. It stays the parity oracle.
- **Speculatively mutate the served graph and roll back.** Breaks the single-writer snapshot contract.
- **A new storage format for obligations.** Checkpoint sidecars already give durability, overlay, anchoring and gone tracking (#118).

## Gates (N1)

- **Phase 0 (before any code).**
  - **Data:** replay merged PRs with at least 2 commits via `refs/pull/N/head`. Most PRs were squash-merged: 8 merge commits out of 231, and 36 of 104 PRs have more than one commit.
  - **Fixed up front:** `max_depth` 3 and a union impact set; first-commit files excluded from both targets and baselines; co-change history strictly before the merge base; PRs the preview would reject excluded.
  - **Test:** a paired PR-cluster bootstrap with at least 15 PRs holding a later-needed code file. Build only if the preview beats same-size same-directory and co-change baselines, with CI95 excluding zero.
- **Phase 2 (the ledger).** Starts only after the preview op ships, has been used in real sessions, and the owner agrees that impact output is worth persisting.
