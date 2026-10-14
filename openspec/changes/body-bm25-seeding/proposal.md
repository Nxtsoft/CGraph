## Why

Free-text queries (stack traces, review comments, issue text) name behavior, not symbols. The
lexical fallback scores only symbol labels (`stem_terms(lexical_terms(node.label))` in
`lexical_matches`), so a query whose words live in a function's body -- the identifiers it calls,
the strings it logs, its comments -- resolves the wrong seeds or nothing at all. Agent Retrieval
Bench (ARB) queries are exactly this shape (CGR-25).

## What Changes

- New runtime-only `BodyIndex` (`src/engine/body_index.cpp`): Okapi BM25 (k1 = 1.2, b = 0.75)
  over the stemmed lexical terms of every code symbol's source span. Each file is tokenized once
  and each node's document is cut from that stream by line range; files are read, hash-verified
  and tokenized concurrently.
- A file is indexed only when its bytes hash to the snapshot's recorded source hash, so a span is
  never cut from bytes other than the ones extraction parsed. A snapshot without source hashes
  (a bare `graph.json` load) has an empty index and ranks by label alone.
- Maintained on the single-writer path: `publish_graph_snapshot` attaches the index to every
  snapshot, reusing a previous index's per-file entry whenever the file's hash and its indexed
  node spans are unchanged. An incremental update re-reads only the files it re-extracted;
  republishing an unchanged snapshot (overlays, memory) returns the previous index as is. The
  daemon's staged rescans (`update`, the post-fast-load hydration) start from the served snapshot
  so they reuse it too.
- Query routing in the lexical fallback (`free_text_matches`, used by both `context` focal/seed
  resolution and the `query` op's lexical route): a query without whitespace is identifier-like
  and keeps the label-only path; a query with whitespace is prose and fuses the label ranking with
  the top 100 BM25 body matches by reciprocal rank (k = 60). Graph expansion is unchanged.
- Publish cost: every snapshot carries a `code_generation` (fresh per built snapshot, kept by
  copies). `publish_graph_snapshot` skips the index entirely when the snapshot's index was
  verified for its own generation -- overlay, memory and semantic-replay mutations of a published
  copy -- so those publishes do no per-node work. Code that edits code nodes or code hashes on a
  copy assigns a fresh generation (the fast-load path does).
- A file whose read or tokenization throws is skipped (the worker never lets it escape) and
  listed in `status.body_index.failures`; `status.body_index` also reports `files`, `documents`,
  `files_failed`.
- `query` search route: `total` keeps counting name matches; body-only candidates are counted in
  `body_candidates`. An unresolved structural phrase (`callers of X`) scores only `X` on bodies.
- `fuse_by_reciprocal_rank` takes any number of rankings, so an optional embedding provider can
  join as one more list later; the binary stays model-free.
- `lexical_terms`/`stem_term`/`stem_terms` move from `daemon_ops.cpp` to `body_index.cpp` so the
  index and the label path share one tokenizer. Output is identical to the old implementation:
  0 mismatches over 1,623,545 lines / 5,937 files compared line by line and file by file.
- Non-goals: embeddings; persisting the index (nothing new is written to `cgraph-out`, so the
  fast-load index key is unchanged); indexing memory or enrichment nodes.

## Measured effect

ARB dev subset (40 positive samples, official `target_gold_files`/leakage filter, file-level
`recall_at`/`reciprocal_rank` from ARB's `baseline.py`; clap 10, caddy 13, gin 17 across 18
snapshots; same adapter for both engines: `context` q=query, budget 8000 focus+included files in
order, then `query` limit 100):

| engine | MRR | Recall@5 | Recall@20 |
| --- | --- | --- | --- |
| label-only (main 42d8501) | 0.3042 | 0.3500 | 0.6250 |
| body BM25 fused (this change) | 0.3873 | 0.4458 | 0.7667 |
| RepoMap, same 40 ids (ARB's published details) | 0.3319 | -- | 0.7375 |

Committed retrieval gate (`retrieval_quality_test`): passing at 0.2616 / 0.3532 / 0.3998 / 0.4387
(the gate slices live worktree bytes for snippets, so these move with edits to the tree itself:
0.3984 / 0.4379 at 6000 / 8000 on the first commit of this change). Its fixture graph carries no
source hashes, so the body route is inert there. Rebased onto the fixture's own commit (0cb8237) with hashes computed, the
same rows measure label-only 0.2541 / 0.3571 / 0.3943 / 0.4379 against fused 0.2884 / 0.3459 /
0.4318 / 0.4726 (the 4000 dip, -0.011, is inside kTol = 0.03).

CGraph repo (origin/main tree, 3566 nodes), daemon, interleaved runs: cold build median 0.690 s
to 0.745 s (+8%, quiet machine, 10 runs each) and 0.745 s to 0.850 s (+14%, load rising to 12,
10 runs each); warm `context` p50 prose 14.64 to 14.51 ms and identifier 12.72 to 12.28 ms; RSS
after build 102.8 to 105.2 MB. `BodyIndex::refresh` itself: about 27-35 ms full, about 5 ms
when every file is reused; `rank` about 14-23 us per query.

Publish cost on the turing-webapp copy (23,531 nodes, 2,113 indexed files), previous PR head vs
this change vs main 42d8501, daemon via `cgraph-client`, 200 attributed semantic drops present:

| measure | main | PR before fix | PR after fix |
| --- | --- | --- | --- |
| `remember`, no drops (median of 10) | 92 ms | 125 ms | 91 ms |
| `remember`, 200 drops (median of 10) | 88 ms | 134.5 ms | 90.5 ms |
| `update` op, 200 drops (median of 5) | 36.5 s | 45.5 s | 35.8 s |
| incremental edit to applied, 200 drops (median of 3, 2 s poll) | 42.5 s | 48.9 s | 41.4 s |

In-process: an unchanged publish costs 0.003 ms (the per-node pass it replaces: 39-42 ms); a
one-file span change publishes in 100-105 ms (one file re-tokenized, postings re-aggregated).
The 36-41 s overlay replay with 200 drops is the same on main.

## Impact

- `src/engine/body_index.cpp`, `src/engine/include/cgraph/body_index.hpp` (new).
- `src/engine/include/cgraph/types.hpp` -- `GraphSnapshot::body_index` (runtime-only).
- `src/engine/daemon_ops.cpp` -- `free_text_matches`, `fuse_by_reciprocal_rank`, publish hook.
- `src/engine/daemon_server.cpp` -- staged rescans start from the served snapshot.
- Tests: `tests/smoke/body_index_test.cpp` (new), `tests/smoke/daemon_ops_test.cpp`.
- Capability: `graph-daemon-client`. `graph.json`, exports, IDs and the fast-load key are
  untouched (Graphify parity holds).
