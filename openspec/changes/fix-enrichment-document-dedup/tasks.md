# Tasks

## 1. Decide the identity rule (do this first)

- [ ] 1.1 Decide whether enrichment kinds (`document`, `media`, `concept`) may
      fuzzy-merge across files at all. Code symbols must keep doing so --
      `tests/smoke/dedup_test.cpp` asserts the cross-file `PaymentService` /
      `Payment Service` merge as Graphify parity behaviour.
- [ ] 1.2 Record the decision and its rationale in the proposal before writing code.

## 2. Implement

- [ ] 2.1 In `src/engine/dedup.cpp`, refuse a fuzzy merge between two nodes that
      record different `source_file` values and no `source_location`.
- [ ] 2.2 Confirm the existing guards still hold: identical labels from a file stay
      apart, overload sets stay apart, same-site duplicates still merge, concepts
      still merge, and a site-less node never merges with a sited one.

## 3. Prove it

- [ ] 3.1 Re-run the harness over `cgraph-out/graph.json`. Expect 1773 nodes and
      3327 edges preserved (from 1728 / 3183), and none of the four measured pairs
      collapsing.
- [ ] 3.2 Add a regression test with two similar document labels in different
      files. The suite already builds documents carrying a `source_file`
      (`semantic_ingest_test.cpp`, `host_surface_integration_test.cpp`) but never
      fuzzy-dedups two similar ones -- that gap is why this shipped.
- [ ] 3.3 Verify the test fails when the new guard is removed.
- [ ] 3.4 `ctest --preset default` and `--preset sanitizers` green.

## 4. Consider tightening the fragment contract

- [ ] 4.1 `src/engine/fragment_json.cpp` parses `source_file` and `source_location`
      independently and `semantic_fragment_validation.cpp` imposes no rules on
      either, so all four presence combinations are reachable for any kind and
      `"source_location": 42` degrades silently to absent. Decide whether the
      contract should constrain this per kind, or whether dedup should stay
      defensive about every combination.
