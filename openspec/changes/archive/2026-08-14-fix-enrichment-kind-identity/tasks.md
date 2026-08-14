# Tasks

## 1. Reproduce + regress
- [x] 1.1 Regression in `tests/smoke/semantic_ingest_test.cpp`: ingest a planned `.md` source with a
      document node whose `type` is omitted / aliased (`doc`); assert the merged node's kind is
      stamped `document`. Media source -> `media`. A `concept` node carrying a `source_file` is
      preserved; a node whose `source_file` is not a planned source keeps its host kind. Verified
      the four stamping assertions fail with the stamp disabled.
- [x] 1.2 Capstone through the real ingest -> dedup path: two kind-less documents from different
      files with similar labels (JW ~0.94) both survive `semantic_dedup` after ingest stamps them.

## 2. Implement
- [x] 2.1 In `src/engine/semantic_ingest.cpp`, add `#include "cgraph/file_watcher.hpp"` and a
      `stamp_planned_kinds` helper; call it after `current_sources` is built, before merge. For each
      node whose normalized `source_file` is a planned source, set `node.kind` to the
      `classify_watched_file`-derived kind (`document`/`media`), overriding the host kind unless the
      host explicitly wrote `concept`.
- [x] 2.2 Confirm concept nodes, code symbols, and nodes with unplanned source_files are untouched;
      confirm a host-correct kind is idempotent.

## 3. Prove it
- [x] 3.1 Full `ctest --preset default` green; sanitizers via CI.
- [x] 3.2 End-to-end proof via the capstone test: `ingest_semantic_fragment` (the exact function
      the daemon calls, with plan-shaped `SemanticSourceInput` sources) followed by real
      `semantic_dedup` -- two kind-less cross-file documents both survive. Live daemon smoke: a
      running `graphd` stays healthy through kind-less document drops (clean log, clean idle exit).
      A manual drop with no prior plan uses the daemon's self-attribution path and is intentionally
      not stamped (the orphan path is out of scope by design).
- [x] 3.3 Escape path B (omitted `source_file`) recorded as a residual in the proposal;
      validation-side `source_file` requirement for document/media parked as a follow-up.
