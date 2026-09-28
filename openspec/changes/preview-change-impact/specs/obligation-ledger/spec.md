## ADDED Requirements

### Requirement: A preview can record its impact as obligations
When `preview` is called with `record: true`, the daemon SHALL store one obligation per impacted consumer or test node. Each obligation SHALL be a memory node in the `memory:obligation:` namespace, persisted as a sidecar fragment under `cgraph-out/memory/`. It SHALL carry a `concerns` edge anchored to the consumer's source span (as checkpoints are), the witness chain, the diff's per-file base and new hashes, and a `class` of `consumer` or `test`.

#### Scenario: Recording creates durable obligations
- **WHEN** a preview with `record: true` lists three dependents
- **THEN** three obligations exist, and after a daemon restart `graph_obligations` still lists all three

### Requirement: Obligation state is derived from the code, not stored
`graph_obligations` SHALL report each obligation's state as: `proposed` when every diff file still hashes to its base hash; `open` when the change has landed and the consumer's span still matches its anchor; `revisited` when the consumer's span no longer matches its anchor; `gone` when the consumer no longer resolves. The engine SHALL NOT infer that an obligation is satisfied.

#### Scenario: The lifecycle of one obligation
- **WHEN** an obligation is recorded, then the diff is applied, then the consumer's own lines are edited, then the consumer is deleted
- **THEN** its state reads `proposed`, `open`, `revisited` and `gone` in turn

### Requirement: Only the host resolves an obligation
The daemon SHALL expose `obligation_resolve` (MCP `graph_obligation_resolve`) accepting an obligation id, a status of `satisfied` or `waived`, and a note, and SHALL persist the resolution in the obligation's sidecar. A resolved obligation SHALL read `stale` when the consumer's span changes after the resolution.

#### Scenario: A resolution goes stale when the code moves on
- **WHEN** an obligation is resolved `satisfied` and the consumer's lines later change
- **THEN** it reads `stale`

### Requirement: Obligations are inert to analysis and retrieval
Obligation nodes SHALL follow the existing rules for `memory:` nodes. They get no centrality or community, never enter `query` or `context` candidates, and are excluded from `graph.json`.

#### Scenario: Obligations never enter retrieval
- **WHEN** `context` runs on a consumer that has open obligations
- **THEN** no `memory:obligation:` node appears among its candidates
