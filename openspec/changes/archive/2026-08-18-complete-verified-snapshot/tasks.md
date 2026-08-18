## 1. Snapshot source evidence

- [x] 1.1 Add failing paired tests for runtime source-hash ledger population on full/incremental rebuild and persisted fast-load reconstruction.
- [x] 1.2 Implement the internal snapshot source-hash ledger without changing graph JSON or deterministic graph parity.
- [x] 1.3 Add failing paired tests for request-local read/hash/slice behavior, same-file reuse, missing evidence, and changed-byte rejection.
- [x] 1.4 Implement request-local source verification and fail content-root-pinned snippet-producing reads atomically on missing or mismatched evidence.
- [x] 1.5 Add and pass a real daemon synchronization/edit/pinned-read test proving that no old-graph/new-source response is served.

## 2. Dependency-aware semantic cache

- [x] 2.1 Add failing paired tests for semantic cache v2 round-trip, duplicate-content source paths, fragment hashes, dependency fingerprints, rejection reasons, and unsupported-schema invalidation.
- [x] 2.2 Implement semantic cache v2 with composite source identity, fragment SHA-256, external code dependencies, and current error state.
- [x] 2.3 Add failing paired tests for planner treatment of valid, stale, and failed current records and preservation of unrelated cache hits.
- [x] 2.4 Implement dependency reconciliation, fragment-level replay gating, and precise requeue behavior before semantic overlay replay.
- [x] 2.5 Add failing paired tests for current-state enrichment counters and successful replacement clearing prior failure.
- [x] 2.6 Replace cumulative failure accounting with current plan/cache-derived health and persist every failed or recovered source transition atomically.
- [x] 2.7 Add and pass a real daemon lifecycle test covering ingest, code-target deletion, graph update, invalid-overlay omission, requeue, replacement, restart, and stable recovery.

## 3. Integration and verification

- [x] 3.1 Run targeted snapshot, persistence, daemon-op, semantic-cache, semantic-plan, semantic-ingest, and daemon-server tests.
- [x] 3.2 Run the complete default test suite, sanitizer suite, graph parity/golden tests, and end-to-end retrieval gate.
- [x] 3.3 Run a warmed pinned-context latency comparison and verify source hashing is bounded to selected distinct files with no material regression.
- [x] 3.4 Run OpenSpec validation, record the real-flow evidence in the PR, and ensure every task and acceptance scenario is satisfied. Closing this task surfaced three engine defects the checked suite runs had missed (serve-loop deadlock under a paused replay, non-atomic overlay publish, unreadable-source cache poisoning); all three are fixed in this change and covered by the previously-failing tests.
