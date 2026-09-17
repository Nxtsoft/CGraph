# Tasks

## 1. Fingerprints
- [x] 1.1 `fingerprint_test.cpp`: renamed TypeScript copies fingerprint identically (tokens and
      shingles); two inserted statements lower similarity without erasing it; an unrelated body is
      near 0; a one-liner is fingerprinted but under the floor; shingle sets sorted and unique;
      winnowing bounds; determinism across files; Python copies with a comment identical; two empty
      fingerprints are 0.
- [x] 1.2 `FunctionFingerprint`, `Fragment::fingerprints`, `GraphSnapshot::fingerprints`;
      `fingerprint.hpp/.cpp` (`normalized_tokens`, `fingerprint_function`, `fingerprint_similarity`).
- [x] 1.3 `extractor.cpp` fingerprints every function node's body; `graph_builder.cpp` unions
      fingerprints in both merge paths; `CMakeLists.txt` lists the new source and test.
- [x] 1.4 `daemon_lifecycle.cpp` persists `fingerprints.json` beside `graph.json` and loads it;
      `daemon_lifecycle_test.cpp` round-trips a fingerprint through persist/load.

## 2. Clones view
- [x] 2.1 `report_test.cpp::test_clones_view`: totals (functions, fingerprinted, eligible), the hint,
      one production class of the identical copies with extents, the test class bucket, threshold and
      min_tokens widening, include_tests merging, scope, shedding order (test classes first), json and
      markdown shapes.
- [x] 2.2 `report_test.cpp::test_clones_envelope`: json and markdown envelopes, diagram formats
      refused, negative min_tokens rejected; `clones` leaves the reserved list.
- [x] 2.3 `build_clones_report`, `shed_to_budget`, `clones_report_json`, `render_clones_markdown`,
      `render_clones_report`, `report_response` dispatch.

## 3. Surfaces, docs, verification
- [x] 3.1 CLI `cgraph report clones` + `--min-tokens` + hint on stderr; MCP `graph_report` + `min_tokens`;
      host contract, skill, READMEs, CLAUDE.md.
- [x] 3.2 `ctest --preset default`: 78/78 pass (this Mac, Debug preset).
- [x] 3.3 Measured on CGraph's own tree from the built daemon: 27 production / 14 test classes, 114
      members, under a second; top classes are genuine copies (`node_text` x4, `source_location` x4,
      `is_*` x6, `count_*` x3); `write_file` (16-23 token bodies) is under the default floor, and all
      35 group at `min_tokens` 10 (18 at 1.00, 8 at 1.00, 9 at 0.83); one-shot build 733-908 ms vs
      751 ms before.
- [x] 3.4 Measured on `frontend` (1,261 TypeScript files, 5,749 functions): 216 classes, 6 test classes,
      614 members; 1.6-2.5 s per call after the upper-bound prune (8 s before, same classes); default
      budget keeps 71 classes.
- [x] 3.5 Restart test: fast-load serves 999/999 fingerprints without the hint; with the sidecar removed
      0/999 with the hint; `update .` rehydrates to 999/999.
- [ ] 3.6 Obtain non-author review and merge.
