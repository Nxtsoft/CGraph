## Why

Verified graph freshness currently covers deterministic code structure but not every query-visible byte or semantic overlay. A content-root-pinned context read can load newer source bytes from disk, while a cached semantic fragment can remain marked valid after its referenced code nodes disappear; both cases can make an agent trust evidence that does not belong to the selected snapshot.

## What Changes

- Make every source snippet returned by a pinned graph read verifiably belong to the selected graph snapshot; fail the read without graph data when source bytes no longer match.
- Persist semantic fragment dependencies on deterministic code inputs and revalidate only affected fragments whenever the deterministic graph is rebuilt.
- Atomically mark rejected or dependency-invalid fragments stale/failed, requeue their source inputs, and clear the state after a successful replacement.
- Derive enrichment health from current cache records and current planning state instead of a cumulative failure-event counter.
- Preserve cache hits for semantic fragments whose sources and referenced code dependencies are unchanged.
- Add real daemon update, edit, restart, and recovery tests for both trust boundaries.
- Non-goals: retaining historical source blobs, globally invalidating semantic fragments on every code edit, changing deterministic graph identity, adding model/provider logic, or making ordinary unpinned reads perform a full-project verification.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `graph-daemon-client`: Content-root-pinned reads that return source snippets must verify those bytes against the selected graph snapshot and fail closed on mismatch.
- `semantic-fragment-ingest`: Semantic cache validity and enrichment health must include current deterministic-code dependencies, rejected fragments must re-enter planning, and successful replacement must clear current failure state.

## Impact

- Snapshot model, extraction-index persistence, daemon context/explain/query response paths, and graph-read error behavior.
- Semantic cache schema and migration-by-rebuild, chunk planning, fragment ingest/replay, enrichment status, and drop-manifest metadata.
- Paired source tests plus real daemon lifecycle tests; no new runtime dependency or model invocation.
