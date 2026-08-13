## MODIFIED Requirements

### Requirement: Enrichment state visibility
The system SHALL expose current semantic enrichment state as idle, pending, running, stale, or failed for relevant files and for the graph as a whole. Pending, stale, and failed counts SHALL be derived from current source/cache records and the current plan rather than cumulative processing events.

#### Scenario: Stale enrichment is visible
- **WHEN** a semantic input or one of its deterministic code dependencies changes after enrichment was valid
- **THEN** status reports the affected current input as stale or pending without blocking deterministic graph queries

#### Scenario: Current failure is visible
- **WHEN** the current fragment for a semantic source is rejected
- **THEN** status reports that source as failed with a current rejection reason and keeps it eligible for planning

#### Scenario: Successful replacement clears failure
- **WHEN** a failed source receives a valid replacement fragment whose dependencies resolve
- **THEN** the current failed count decreases, the source becomes valid, and graph-level state no longer remains failed because of the prior attempt

### Requirement: Incremental semantic cache
The system SHALL store semantic enrichment results by normalized source path and source content hash. A reusable record SHALL also bind the exact fragment hash, cache schema version, and every referenced external deterministic code node's ID, source path, and source SHA-256. The system SHALL invalidate only records whose source, fragment, schema, or recorded code dependency no longer matches.

#### Scenario: File edit invalidates semantic cache
- **WHEN** a semantic source file's content hash changes
- **THEN** the prior semantic fragment is marked stale and the file is eligible for the next chunk plan

#### Scenario: Referenced code node disappears
- **WHEN** a valid semantic fragment references a deterministic code node that is absent after a graph update
- **THEN** the affected record is marked stale, its source re-enters the next plan, and the fragment is not replayed

#### Scenario: Referenced code source changes
- **WHEN** a referenced code node remains addressable but its exact parsed source hash or normalized source path changes
- **THEN** the affected semantic record is marked stale and requeued for fresh authoring

#### Scenario: Unrelated code edit preserves cache hit
- **WHEN** a graph update changes a code file that is not recorded by a valid semantic fragment
- **THEN** that fragment remains valid, is replayed, and its semantic source remains a cache hit

#### Scenario: Fragment bytes change independently
- **WHEN** a dropped fragment's SHA-256 differs from the hash stored in its current cache records
- **THEN** those records are not reused and the affected semantic sources are requeued

#### Scenario: Unsupported cache schema is not reused
- **WHEN** the daemon reads a semantic cache without the current dependency-fingerprint schema
- **THEN** it treats the cache as having no reusable records and reconstructs current records only from newly validated ingestion

### Requirement: Fragment edges must resolve to known nodes
Semantic ingest SHALL reject a fragment atomically—with the graph unchanged—when any edge endpoint resolves against neither the fragment's own nodes nor the selected deterministic graph snapshot. Rejection SHALL write a failed current cache record with the unknown endpoint for every source mapped to the fragment so those sources remain eligible for planning.

#### Scenario: Dangling edge endpoint
- **WHEN** a schema-valid fragment carries an edge whose target ID exists in neither the fragment nor the graph
- **THEN** the fragment is rejected with an error naming the unknown endpoint, no node or edge from the fragment enters the graph, and each mapped current source is marked failed and pending for replacement

#### Scenario: Edges into the existing graph still merge
- **WHEN** a fragment's edge references a node already present in the graph snapshot
- **THEN** the fragment merges normally and records that node's ID, source path, and source SHA-256 as a dependency

## ADDED Requirements

### Requirement: Dependency-valid semantic replay
After every deterministic graph rebuild, the daemon SHALL reconcile semantic cache dependencies before replaying overlays. It SHALL replay a dropped fragment only when its fragment hash and every current mapped source record are valid against the rebuilt graph.

#### Scenario: Rebuild omits invalid overlay
- **WHEN** a deterministic rebuild removes a code dependency of a previously valid fragment
- **THEN** the published graph contains no node or edge from that invalid fragment and its source is present in the next semantic plan

#### Scenario: Restart reproduces reconciled state
- **WHEN** the daemon restarts after dependency invalidation
- **THEN** it reports the same current stale/failed/pending state and does not restore the invalid overlay

#### Scenario: Replacement recovers atomically
- **WHEN** the host drops a valid replacement for all mapped sources of an invalid fragment
- **THEN** the replacement is merged, fresh dependency fingerprints are persisted, prior current errors clear, and a subsequent restart reproduces the recovered graph and health state
