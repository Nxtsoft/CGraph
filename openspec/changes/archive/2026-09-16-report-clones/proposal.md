# Feature: function fingerprints and `report clones` (CGR-10)

## Why

"Similarity matching for function blocks to identify 80% similar functions" was the third of the
four asks that started the CGR-7 track. jscpd, similarity-ts, surgex and fallow all ship an
80%-class near-miss detector this year, every one as a batch report over files; none answers
against a resident graph that already knows the function boundaries, the call edges and the
module structure. CGraph's own tree has the motivating case: `write_file` is defined in 35 test
files, three of them (`content_root_export_test.cpp:14`, `cpp_extractor_test.cpp:11`,
`daemon_persistence_test.cpp:18`) differing only in a namespace alias and an `std::ios::binary`
flag. `dedup.cpp` fuzzy-matches labels, not bodies, so the graph cannot see this.

## What Changes

- **Fingerprints at extraction** (`fingerprint.cpp`, new). For every function node the shared
  walker creates, the body subtree (the config's `body_fields` child; the whole definition when the
  grammar has none) is reduced to a normalized token stream: every identifier becomes `ID`, every
  string/number/character literal becomes `LIT`, comments vanish, keywords, operators and
  punctuation keep their text. Every run of five tokens is hashed (64-bit FNV-1a) and winnowed
  (window four, minimum hash, rightmost on a tie), giving a sorted, unique shingle set plus the
  token count. `Fragment::fingerprints` and `GraphSnapshot::fingerprints` are runtime-only maps
  keyed by node id, like `source_hashes`: never in a fragment file, never in `graph.json`, so
  Graphify parity holds. `merge_fragments`/`merge_fragment` union them first-occurrence-wins;
  the incremental index carries them with each file's fragment, so a re-extracted file replaces
  its own entries and a deleted file drops them.
- **Persistence.** `persist_graph_snapshot` writes `fingerprints.json` beside `graph.json` and
  `load_graph_snapshot` reads it back, so a fast-loaded daemon serves clones immediately. The
  index version key is deliberately not bumped: an older persist has no sidecar, the graph loads
  with empty fingerprints, and the clones report says so in a `hint` (with the `update .` rescan
  that computes them) rather than forcing every project into a cold rebuild on upgrade.
- **`report clones`**. Function nodes in scope with a fingerprint of at least `min_tokens`
  (default 30) tokens are candidates. Candidate pairs come from an inverted index on shingle hash,
  skipping any shingle shared by more than 512 functions (boilerplate every body has); each pair's
  intersection is counted once, so the Jaccard is exact and no pair without a common shingle is
  visited. Pairs at or above `threshold` (default 0.80) are united into classes. A class carries
  its members (root-relative file, line range, label, tokens; by file then line), its lowest
  pairwise `similarity` and its shortest body's `tokens`. Classes whose members all lie under test
  roots are `test_classes` unless `include_tests` merges them: duplicated fixtures are the
  commonest clone and the least urgent, so they are reported after production classes and shed
  first. Ordering: largest class, then most similar, then longest. Formats `json` and `markdown`;
  `mermaid`/`svg` are refused with `report_format_unsupported`. `design` stays reserved.
- Surfaces: `cgraph report clones` (markdown default, `--min-tokens`, the `hint` on stderr), MCP
  `graph_report` (view, `min_tokens`, description), host contract, bundled skill (a routing row
  for copy-paste questions), READMEs, CLAUDE.md pipeline note.

### Non-goals
- Type-1 exact-text detection as a separate mode: an exact copy is a 1.00 class here.
- Clones that span less than a whole function (a repeated block inside two larger bodies). The
  unit is the function node the graph already has.
- Cross-language clones: the normalized streams keep each grammar's keywords and punctuation, so
  a Python and a TypeScript copy of the same routine do not match, by design.
- A refactoring suggestion. The class lists where the copies are; what to extract is the reader's
  call.
- Semantic (embedding) similarity: model logic stays out of the binary per CLAUDE.md.

## Impact

- **Touches:** `src/engine/include/cgraph/types.hpp`, `src/engine/include/cgraph/fingerprint.hpp`
  (new), `src/engine/fingerprint.cpp` (new), `src/engine/extractor.cpp`,
  `src/engine/graph_builder.cpp`, `src/engine/daemon_lifecycle.cpp`, `src/engine/CMakeLists.txt`,
  `src/engine/include/cgraph/report.hpp`, `src/engine/report.cpp`, `src/cli/main.cpp`,
  `src/mcp/mcp_server.cpp`, `tests/smoke/fingerprint_test.cpp` (new), `tests/smoke/report_test.cpp`,
  `tests/smoke/CMakeLists.txt`, docs.
- `graph.json`, fragment files and every export are byte-identical; `fingerprints.json` is a new
  sidecar next to the daemon's persisted graph.
- Extraction cost: one extra subtree walk per function. CGraph's own tree (193 files) builds in
  733-908 ms with fingerprints against 751 ms without, within run-to-run noise; `graph.json` has
  no `shingles` key. The persisted sidecar for it is 0.96 MB.
- Measured on CGraph's own tree (999 functions, all fingerprinted, 765 at the 30-token floor),
  defaults, under a second per call: 27 production classes and 14 test classes, 114 functions
  in a class. The top classes are real copies: `node_text` x4 and `source_location` x4 pasted
  across the C++, generic, JavaScript and Python extractors (1.00), the six `is_*` set-membership
  predicates (1.00), `count_valid`/`count_stale`/`count_failed` (1.00), the four five-line
  `extract_<language>` wrappers (1.00, exactly 30 tokens), `csharp_config`/`scala_config`/
  `groovy_config` at 0.85. The largest test class chains 14 `has_*` helpers whose lowest pair is
  0.56: union-find joins through 0.80 pairs, and the class reports its weakest link so a loose
  chain is visible.
- The plan's acceptance example, the 35 `write_file` test helpers, is **below the plan's own
  floor**: their bodies are 16-23 tokens. At `min_tokens` 10 all 35 group into three classes (18
  at 1.00, 8 at 1.00, 9 at 0.83); at the default 30 none do, by design. The first implementation
  also missed them at any floor because the hot-shingle skip was applied while counting the
  intersection, scoring identical copies 5/7; the Jaccard is now computed exactly over both full
  sets once a pair is a candidate, and `report_test.cpp` pins the case.
- Measured on the `frontend` Next.js app (1,261 TypeScript files; 5,749 functions, all
  fingerprinted after a 160 s `update .` rescan; 4,261 at the floor), defaults: 216 production
  classes and 6 test classes, 614 functions in a class; sizes 154 pairs, 32 triples, up to 17.
  The largest are the copy-pasted request wrappers in `lib/*-api.ts` (17 `create`/`update`
  bodies at 0.82 lowest pair; 15 at 0.71; 14 `getById`-shaped at 0.76), nine identical
  `fetch*` hooks at 1.00, and a 16-member chain of `use*Tag`/`use*Notebook` mutation hooks
  whose weakest pair is 0.58. One class is a minified Playwright trace asset
  (`playwright-report/trace/assets/*.js`, nine one-line members): a build artifact the detector
  should skip, filed as a follow-up. The unbudgeted JSON is ~41k tokens; the default budget keeps
  71 classes (~6k tokens) and reports 145 omitted. Wall time: 8 s before the upper-bound prune,
  1.6-2.5 s after, with identical classes.
- Persistence, measured: after a daemon restart the fast-loaded graph serves 999/999 fingerprints
  with no hint; with `fingerprints.json` removed it serves 0/999 with the hint; `update .`
  rehydrates to 999/999. The frontend daemon, fast-loaded from a persist written by the previous
  build, reported the hint for all 5,749 functions, as designed.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — every function node carries a runtime fingerprint.
- `graph-daemon-client` — the `report` op serves a `clones` view; fingerprints persist beside the graph.
- `host-integration-mcp` — `graph_report` exposes the clones view and `min_tokens`.
