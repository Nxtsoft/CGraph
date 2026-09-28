# Design: preview-change-impact

## Test strategy first

| Behaviour | How it is tested |
|---|---|
| Preview equals change_context | Parity test in `tests/smoke/preview_test.cpp`: a fixture repo plus a diff, the preview run in memory against `change_context` run with the target materialized in a temp dir. `changes` and the impact ids must be identical. |
| Nothing is written or published | The same test records the served snapshot pointer, `content_root` and directory mtimes before and after, and asserts all are unchanged. |
| Rejections | Old side mismatch, a file edited after extraction (the #118 `source_hashes` gate), overlapping hunks, and a path outside the root. Each returns an error and no partial result. |
| Only touched files re-extract | The preview reports `files_reextracted` and `files_cache_hit`. The test asserts reextracted == diff files. |
| Obligation states | `tests/smoke/daemon_ops_test.cpp`, in the style of the #118 block, using real files. Record, then the diff is still unapplied (`proposed`). Apply it (`open`). Edit the consumer's own lines (`revisited`). Delete the consumer and rebuild with overlay (`gone`). Resolve (`satisfied`). Restart via sidecar overlay, and the states survive. |
| Inertness | Obligation nodes get no centrality or community, and never reach `query` or `context`. This extends the existing memory inertness tests. |
| Real flow | A live `graphd` via `cgraph-client` on a scratch project. Preview a deleting diff and see the callers. Record, apply the diff with `patch`, fix one caller, and list the obligations: one `revisited`, the others `open`. |
| Mutation check | Removing the overlay's re-extraction fails parity. Removing the freshness gate fails the rejection test. |

## Preview: building the branch

```
agent diff text ──► parse (unified_diff) ──► validate old side against
                                              • live file bytes
                                              • served snapshot source_hashes  (reject: "graph not caught up; run graph_update")
                                          ──► reconstruct new contents in memory (apply_patch)
                                          ──► copy index.files under writer_mutex; release
                                          ──► overlay: for each touched path
                                                 modified/added → extract_detected_source(path, new_contents)
                                                 deleted        → drop entry
                                          ──► rebuild_graph(overlay)  (private branch GraphSnapshot)
                                          ──► symbol changes + trace_impact on base and branch
                                              (same code path as change_context's add_side)
                                          ──► budgeted response; branch discarded
```

- **Extraction from memory.** `extract_detected_file` reads the file and passes `source` into the extractor (`file_extraction.cpp:68,75`). A sibling `extract_detected_source(file, contents, root)` skips the read. Both share one body, so ids and fingerprints cannot drift.
- **Index overlay.** `IncrementalGraphIndex::files` maps each key to an `ExtractionResult`. The preview copies the map under `writer_mutex` (a copy of values that already exist, with no re-parse) and releases the lock before rebuilding, so the serve loop and watcher never wait on a preview. If the copy turns out to be too expensive on large repos, the fallback is a copy-on-write overlay type passed to a `rebuild_graph` overload. That decision rests on the measurement in task 3.4, not on a guess.
- **Freshness.** The #118 rule applies: a file edited after the served snapshot extracted it has stale line numbers, so the preview refuses rather than guesses.
- **Budget and shape.** The preview reuses `change_context`'s response builder and shedding (impact witnesses first, then context), so both ops speak one schema. `path` checks (`paths: [[from, to], ...]`) run on the branch and report whether each connection survives the change.
- **Latency target.** Stated before measuring, and checked in task 3.4: on this repository (about 250 files) a one-file preview completes within the same order as an incremental update. If it does not, the overlay changes before the op ships.

## Obligation ledger

An obligation is a memory node `memory:obligation:<stamp>` with `kind: "obligation"`. It is stored exactly like a checkpoint: a sidecar fragment under `cgraph-out/memory/`, re-overlaid after every rebuild, and excluded from `graph.json`. It carries:

- `concerns` edge to the consumer or test node, with `touch` and `anchor_sha256` (#118 anchoring, including the freshness gate).
- `witness`: the dependency chain from the changed symbol to the consumer (ids plus relations), taken from the preview's impact trace.
- `change`: for each diff file, its base hash and its reconstructed new hash, plus the diff's sha256.
- `class`: `consumer` or `test` (the file lies under a detected test root, or the node is a test).

### Derived states, computed at read time and never stored

| state | condition |
|---|---|
| `proposed` | every diff file still hashes to its base hash in the served snapshot |
| `open` | some diff file differs from its base hash, and the consumer's span still matches its anchor |
| `revisited` | the consumer's span no longer matches its anchor. The engine cannot say whether the edit was right. |
| `gone` | the consumer no longer resolves (the #118 `gone_touches` path) |

### Host-asserted states, stored as a resolution record in the sidecar

| state | set by |
|---|---|
| `satisfied` | `graph_obligation_resolve {id, status: "satisfied", note}`. For `test` obligations the note should carry the content root the tests ran at. |
| `waived` | the same call with `status: "waived"` and a reason |

A resolved obligation becomes `stale` again when the consumer's span changes after the resolution, because the host's claim was about code that no longer exists.

### Surfaces

- `graph_preview {diff, max_depth?, budget?, paths?, record?}`.
- `graph_obligations {status?, class?, limit?}`, newest first, with a count per state.
- `graph_obligation_resolve {id, status, note}`.
- `graph_recall` lists open obligations tied to a checkpoint when both were written in the same task (optional; task 5.4).

## Alternatives considered

- **Materialize the target in a temp dir and call `change_context`.** Zero new engine code, but a full two-root rebuild costs seconds per call. It also writes a tree to disk, so it can never be the pre-edit loop the report describes. It stays the parity oracle.
- **Mutate the served graph speculatively and roll back.** Rejected: it breaks the single-writer snapshot contract, and readers would see a graph that matches no files.
- **Store obligations in a new file format.** Rejected: checkpoint sidecars already give durability, re-overlay, anchoring and gone tracking, all tested (#118).

## Risks

- **Impact noise.** A deep dependents trace can list half the repo. Phase 0 measures precision, `max_depth` defaults to what Phase 0 shows is useful, and the response ranks witnesses by depth and centrality before shedding.
- **Index copy cost** on very large repos (see Index overlay above).
- **Concurrency.** The only shared state is the index copy, taken under `writer_mutex`. A watcher update racing a preview is fine, because the preview's freshness gate compares against the snapshot it copied.
- **Phase 0 bias.** Later commits in a PR mix real misses with review churn. The gate compares preview against baselines on the same data, so churn affects both sides equally.

## Phase gate

Phase 1 (the preview op) starts only if Phase 0 shows the pre-edit impact set catches later-needed code files more often than both same-size baselines, with a CI95 that excludes zero. If it fails, this change is closed with the measurement recorded and no engine code written.
