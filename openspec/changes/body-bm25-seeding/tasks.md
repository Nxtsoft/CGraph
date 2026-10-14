# Tasks

## 1. Index

- [x] 1.1 `BodyIndex`: BM25 over stemmed span terms of every located code symbol; files
      tokenized once, concurrently; a file indexed only when its bytes match the snapshot's
      source hash.
- [x] 1.2 Per-file reuse keyed by source hash + indexed span signature; `refresh` returns the
      previous index unchanged when nothing changed.
- [x] 1.3 `publish_graph_snapshot` attaches it; staged rescans (`update`, hydration) start from
      the served snapshot so they reuse it.

## 2. Routing

- [x] 2.1 `free_text_matches`: identifier-like (no whitespace) keeps the label path; prose fuses
      label and body rankings by reciprocal rank; used by `context` seeds and the `query` op's
      lexical route.
- [x] 2.2 `fuse_by_reciprocal_rank` over N rankings (seam for an optional embedding list).
- [x] 2.3 Shared tokenizer moved to `body_index.cpp`; equivalence with the old
      `lexical_terms`/`stem_terms` checked on 1.6M real source lines (0 mismatches).

## 3. Tests

- [x] 3.1 `body_index_test`: body-only vocabulary ranks the right symbol; nested and final-line
      spans cut exactly; reuse returns the same index; an edit re-tokenizes one file; a re-spanned
      file is re-cut; mismatched bytes and removed files leave the index; no hashes, no index.
- [x] 3.2 `daemon_ops_test`: a prose query resolves a symbol through its body where the label
      path resolves nothing; the `query` op ranks the same way; an identifier-like query stays
      label-only; a republished edit is searchable and the replaced text is not. The existing
      idf-ordering assertion runs on a hash-free snapshot (label path), as it was written for.

## 4. Gate

- [x] 4.1 ARB dev subset (40 samples): MRR 0.3042 to 0.3873, Recall@20 0.6250 to 0.7667.
- [x] 4.2 `retrieval_quality_test` passes unchanged (body route inert on its hash-free fixture);
      at the fixture's own commit with hashes, fused vs label-only stays within kTol.
- [x] 4.3 Warm context p50 on the CGraph repo under 25 ms (prose 14.51 ms); cold build +8% to
      +14% across two interleaved runs; RSS +2.4 MB.
- [x] 4.4 Live daemon: a body edit picked up by the watcher's incremental update is found by a
      prose query 4 s later; a fast-load restart indexes from the manifest's hashes.
- [x] 4.6 Review follow-ups: code-generation fast path (unchanged publish 39-42 ms to 0.003 ms;
      remember and 200-drop update back to main's numbers), per-file exception capture reported in
      `status.body_index`, `body_candidates` apart from `total`, operand-only body scoring for
      unresolved structural phrases, fused-ranking guarantee asserted on the hashed snapshot.
- [x] 4.5 Full ctest: 96/97 pass; `cgraph_file_watcher_test` fails at line 150 (known local-only
      failure, passes in CI).
