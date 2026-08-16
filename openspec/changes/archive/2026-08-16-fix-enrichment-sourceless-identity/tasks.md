# Tasks

## 1. Regress
- [x] 1.1 Add an escape-path-B regression in `tests/smoke/dedup_test.cpp`: two `document` nodes
      with no `source_file`, similar high-entropy labels, sharing a `community` (so the fuzzy pass
      buckets them as candidates at the 0.88 threshold); assert both survive `semantic_dedup`.
      Verified it fails before the fix (merged to 1).
- [x] 1.2 Add a no-over-block fixture: two `document` nodes sharing the SAME real `source_file`
      with different similar labels; assert they still merge to 1 (protects the `.empty()` clause).

## 2. Implement
- [x] 2.1 In `src/engine/dedup.cpp`, change the file-scoped guard to
      `left.source_file != right.source_file || left.source_file.empty()`, and rewrite the stale
      comment.

## 3. Prove it
- [x] 3.1 `cgraph_dedup_test` green; escape-path B fails with the fix reverted.
- [x] 3.2 Full `ctest --preset default` green (66/66); sanitizers via CI.
