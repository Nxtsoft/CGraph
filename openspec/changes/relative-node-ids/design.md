## Context

A node id was `make_id(<absolute source path>[:<label>])`, so the machine that built a graph was
baked into every id and two checkouts of one commit could not be joined by id. This is the first
blocker for the PR-brief plan (https://agents-share.taylor-8a5.workers.dev/tgod009/pr-briefs):
`change-context` pairs base and head symbols by label within a file because their ids never
match.

## The Graphify parity question (task 1)

Task 1.1 asked for a real Graphify export to record whether `_make_id` receives an absolute or a
repo-relative path. Within the ten-minute box the brief allowed, Graphify was not obtainable on
mars: `pip`/`pip3` are not installed, `uvx --from graphify graphify` resolves no PyPI package
("there are no versions of graphify"), `which graphify` finds nothing, and the two source paths
tried on github.com/Graphify-Labs/graphify returned 404 without an authenticated code search.

**Resolution: explicit documented divergence.** Repo-relative ids are adopted regardless of
which path Graphify passes. Sign-off: owner approved 2026-09-25 via the PR-brief plan
(https://agents-share.taylor-8a5.workers.dev/tgod009/pr-briefs), which requires cross-root id
equality. If Graphify passes a relative path this change restores parity; if it passes an
absolute one, cgraph deliberately diverges on the *input* to the normalizer only. `make_id`
itself (`normalize.cpp`) is untouched, so the `ID normalization parity` requirement and its
Unicode fixtures hold by construction (`cgraph_id_normalization_test` unchanged).

## Goals / Non-Goals

Byte-identical ids for the same tree at any absolute root; no id segment above the project root;
identifier-keyed persisted state invalidated rather than mis-resolved; a `change-context` mode
that never sheds a symbol change. Not changing `make_id`, `source_file`, edge semantics, dedup
identity, or the content-root hash.

## Decisions

**Ids derive from a second path on the extraction context; `source_file` stays absolute.** The
proposal assumed `source_file` was already repo-relative because the committed parity fixture
says so, but the fixture README records that its `source_file` values were rewritten after
generation. In a live graph `Node::source_file` is the absolute path: it is the key into
`GraphSnapshot::source_hashes` (`pipeline.cpp:64`), the path every `SnapshotSourceReader` opens,
and what `change_context.cpp:459`, `report.cpp:1096` and the semantic cache relativize or compare.
Making it relative would touch every reader and change the `graph.json`/`context` output shape.
So `ExtractionContext` gains `relative_path` (`language_config.hpp`), set once in
`file_extraction.cpp` from the detected path and the canonical project root, and every id seed
in the extractors reads it instead of `source_file` (28 sites, mechanical). `make_id` is not
touched.

**The root is an explicit argument.** `extract_detected_file(file, project_root)` and
`extract_files(files, project_root)`; the pipeline passes `weakly_canonical(root)` and the
incremental updater passes its already-canonical `index.project_root`. A file outside the root
has no relative path: extraction fails with a warning ("file is outside the project root") and
produces no nodes, never an id that climbs above the root.

**A source path can no longer be normalized back into its file id.** Four sites recomputed
`make_id(source_file)` to find a file node (`graph_builder.cpp` call grading, relation
resolution, `resolve_scoped_name`; `contracts.cpp` endpoint containment). They now look the file
node up by `source_file` through one helper, `file_node_ids(graph)`, carried on
`RelationScopes::file_id_by_source`. `resolve_imports` recomputed `make_id(file:label)` to bind
an import to a declaration; it now binds to the unique declaration of that label in the file
(`declared_by_file_label`), which is what the recomputation approximated.

**File labels use the relative path too.** A root-level file was labelled `<root dir>/main.py`,
leaking the checkout directory into `graph.json`; it is now `main.py`.

**Persisted state.** The fast-load index key moves `logic-4 -> logic-5`, so an index persisted
under the old ids is rejected by the version check and rebuilt (`index_persistence_test`).
Semantic cache records name their dependency by id; a record written under the old scheme is
invalidated with `dependency node missing: <old id>` and never re-pointed at the new node
(`semantic_cache_test`). Memory sidecars store `concerns` edges by target id; `remember` now
writes the agent's `touch` key on each edge, and the daemon's re-overlay calls
`rebind_memory_concerns` first, which re-resolves a missing target through that key and drops
what it cannot rebind, so no dangling edge survives the transition (`daemon_ops_test`).

**`change-context --symbols-only`.** `symbols_only` skips impact tracing, context packing and
budget shedding, returns after the mandatory change classification and the same source
re-verification, and echoes `symbols_only: true`. CLI flag, MCP boolean, usage text.

## Fixture and goldens

Every one of the parity fixture's 1,580 ids carried the dead prefix
`users_taylorgagne_tools_cgraph_agents_worktrees_fixture_reanchor_` (102,700 of 183,941 id
characters, 55.8%). The id a relative-path extraction produces is exactly the old id with that
prefix removed (`make_id` folds the root's separator and a path's leading non-word characters into
one underscore either way), so the fixture pair was rewritten by stripping the prefix from
`graph.json` ids/links and `queries.jsonl` node references: a pure rename, no node added,
removed, merged or regraded (1,580 distinct ids before and after). Ground-truth labels were not
edited.

## Measurements

All numbers from the release build of this branch on mars, same engine, same fixture graph
(1,580 nodes / 3,178 links, 75 symbol rows); "before" is the fixture with prefixed ids, "after"
the rewritten fixture.

| | before | after |
|---|---|---|
| id characters, parity fixture | 183,941 (102,700 = 55.8% prefix) | 81,241 (0% prefix) |
| ids containing an absolute-path segment, self-build of this repo (2,511 nodes) | every id | 0 |

Packing parity (`cgraph_pack_context_parity_test`, mean grade-2 recall, greedy / knapsack):

| budget | before | after |
|---|---|---|
| 2000 | 0.393163 / 0.395117 | 0.414961 / 0.400274 |
| 4000 | 0.487613 / 0.465961 | 0.496701 / 0.470459 |
| 6000 | 0.510741 / 0.516380 | 0.519012 / 0.514780 |
| 8000 | 0.547510 / 0.534375 | 0.549584 / 0.538557 |

End-to-end retrieval (`cgraph_retrieval_quality_test`, grade-2 recall):

| budget | before | after |
|---|---|---|
| 2000 | 0.241528 | 0.257310 |
| 4000 | 0.358842 | 0.353199 |
| 6000 | 0.390547 | 0.398673 |
| 8000 | 0.435563 | 0.440166 |

Both gates were re-pinned to the "after" transcriptions. The checkout-depth environment note in
`pack_context_parity_test.cpp` is gone; `file_extraction_test` and `pipeline_test` pin
two-root id equality directly.

**What still varies with checkout depth.** A context brief's `source_file` is the absolute path
the daemon reads from (`daemon_ops.cpp` `node_brief`). It is not an identifier and this change
does not relativize it: consumers open files through it, and the fixture-based gates cannot
measure it because the committed fixture already carries relative `source_file` values. The
spec delta is scoped to identifiers accordingly.

## Risks / Trade-offs

The first daemon start after upgrade rebuilds every project's fast-load index and requeues every
semantic fragment whose dependencies were recorded by id; that cost is one cold build per
project. Memory checkpoints written before this change carry no `touch` key, so their
`concerns` edges are dropped on the first re-overlay rather than re-resolved; the checkpoint
body and recall are unaffected. Consumers that stored node ids from earlier releases must
re-query.
