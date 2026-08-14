# Tasks

## 1. Decide the identity rule (do this first)

- [x] 1.1 Decide whether enrichment kinds (`document`, `media`, `concept`) may
      fuzzy-merge across files at all. Code symbols must keep doing so --
      `tests/smoke/dedup_test.cpp` asserts the cross-file `PaymentService` /
      `Payment Service` merge as Graphify parity behaviour.
- [x] 1.2 Record the decision and its rationale in the proposal before writing code.

## 2. Implement

- [x] 2.1 In `src/engine/dedup.cpp`, refuse a fuzzy merge between two nodes that
      record different `source_file` values and no `source_location`.
- [x] 2.2 Confirm the existing guards still hold: identical labels from a file stay
      apart, overload sets stay apart, same-site duplicates still merge, concepts
      still merge, and a site-less node never merges with a sited one.

## 3. Prove it

- [x] 3.1 Re-run the harness over `cgraph-out/graph.json`. Expect 1773 nodes and
      3327 edges preserved (from 1728 / 3183), and none of the four measured pairs
      collapsing.
      NOTE: the 1773-node enriched export no longer exists on disk (a later
      one-shot overwrote it), so the harness rebuilds the enriched graph through
      the real path: node-link parse of the current `graph.json` (1563 nodes) +
      `merge_fragment` over the 42 real `semantic-drop/chunk_*.json` fragments ->
      1911 nodes / 4590 edges / 230 documents. Pre-fix dedup deletes 44 documents
      and 144 edges (230 -> 186 docs, 4590 -> 4446 edges) — the same loss the
      proposal measured, including all four listed pairs. Fixed dedup: 230 -> 228
      documents with zero distinct labels lost (the 2 merges are identical-label
      same-file duplicates), media 4 -> 4, concepts 114 -> 113 (label-identical
      concepts merging is by design).
- [x] 3.2 Add a regression test with two similar document labels in different
      files. The suite already builds documents carrying a `source_file`
      (`semantic_ingest_test.cpp`, `host_surface_integration_test.cpp`) but never
      fuzzy-dedups two similar ones -- that gap is why this shipped.
- [x] 3.3 Verify the test fails when the new guard is removed.
- [x] 3.4 `ctest --preset default` and `--preset sanitizers` green.
      NOTE: default green locally (66/66). The sanitizers leg is verified by CI's
      preset matrix: on this Darwin 27 machine EVERY ASan test binary hangs at
      startup — including the pre-change binary already sitting in the main
      checkout's `build/sanitizers` — so the hang is a machine-level ASan
      regression, not this change.

## 4. Consider tightening the fragment contract

- [x] 4.1 `src/engine/fragment_json.cpp` parses `source_file` and `source_location`
      independently and `semantic_fragment_validation.cpp` imposes no rules on
      either, so all four presence combinations are reachable for any kind and
      `"source_location": 42` degrades silently to absent. Decide whether the
      contract should constrain this per kind, or whether dedup should stay
      defensive about every combination.
