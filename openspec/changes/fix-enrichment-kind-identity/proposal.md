## Why

The dedup file-scoped guard that stopped the 44-document / 144-edge deletion
(`dedup.cpp` `enrichment_file_scoped_kind`) protects a node only when its `kind` casefolds to
exactly `document`/`media`. But `kind` is optional and unvalidated (`fragment_json.cpp` reads it
from `type` then `kind`, `semantic_fragment_validation.cpp` imposes nothing) and ingest never set
it. So a host that omits `type`, aliases it (`doc`/`markdown`), or otherwise under-specifies it
silently loses cross-file protection and re-opens the deletion. The protection depended on a field
the host contract (`integrations/skills/cgraph-enrich/SKILL.md`) marks Optional.

The engine already knows the true kind: the plan classifies every enrichment input by path with a
pure function (`classify_watched_file`), and `ingest_semantic_fragment` already receives the
planned source list. The kind was simply being thrown away at ingest.

## What Changes

- At ingest, before merge, stamp the planned kind onto every fragment node whose (normalized)
  `source_file` matches a planned enrichment source. The kind is re-derived from the source PATH
  via `classify_watched_file` -- the same classifier the plan uses -- so it is correct on every
  ingest path (live drop, manifest replay, and restart replay from cache records that carry no
  kind; recomputing sidesteps that a threaded `kind` field would silently stop applying after a
  restart).
- Override the host-written kind, EXCEPT when the host explicitly marked the node a `concept`: a
  concept may legitimately carry a `source_file` (provenance) and is a distinct entity, not the
  document itself, so its identity is preserved. A document a host wrongly labels `concept` is
  thereby left unprotected -- indistinguishable from a real concept, that is an accepted residual.
- Non-goals: no auto-stamping of `source_file` (a `source_file`-less document is
  indistinguishable from a concept; guessing would corrupt concept identity); no change to the
  dedup guard, fuzzy thresholds, or fragment schema. Escape path B (omitted `source_file`) is
  recorded as a residual with a parked validation-side follow-up.

## Impact

- Closes the omit/alias escape path deterministically: any planned document/media node gets the
  true kind, so the identity guard fires regardless of host discipline. The override can only make
  dedup more conservative (block a merge), never cause a wrong one.
- **Touches:** `src/engine/semantic_ingest.cpp` (extend the existing node loop; add
  `#include "cgraph/file_watcher.hpp"`), `tests/smoke/semantic_ingest_test.cpp` (regression +
  ingest->dedup capstone). No struct, cache-schema, or signature change. `node.kind` has no other
  consumer that checks an enrichment kind (`daemon_ops.cpp`, `graph_builder.cpp`, `seam.cpp` only
  check `file`/`module`/`code-ref`), so the override has no blast radius beyond the guard.
- Changes `graph.json` for any enriched project whose hosts under-specify kind (more documents
  survive) -- same direction as the earlier dedup fix.

## Capabilities

### Modified Capabilities

- `semantic-fragment-ingest` -- enrichment-node kind is set from the plan, not the host.
