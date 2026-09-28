## ADDED Requirements

### Requirement: A preview can record its impact as obligations
When `preview` is called with `record: true`, the daemon SHALL store one obligation per impacted consumer or test node, all in one sidecar fragment per recorded preview, written atomically under `cgraph-out/memory/`. Each obligation SHALL be a memory node in the `memory:obligation:` namespace. It SHALL carry a `concerns` edge to the consumer, the witness chain, per-file `{base, new}` hashes (`absent` for added or deleted files), a `class` of `consumer` or `test`, and flags for consumers that exist only on the branch or sit inside a touched file. Recording the same diff against the same base content root again SHALL return the existing obligation ids and write nothing.

#### Scenario: Recording creates durable obligations
- **WHEN** a preview with `record: true` lists three dependents
- **THEN** three obligations exist in one sidecar, and after a daemon restart `graph_obligations` still lists all three

#### Scenario: Recording twice is a no-op
- **WHEN** the same diff is previewed with `record: true` twice against an unchanged base
- **THEN** the second call returns the same ids and no new sidecar is written

### Requirement: Obligation state is derived from the code with a fixed precedence
`graph_obligations` SHALL report each obligation's state as the first of these that holds.
- `gone`: the consumer no longer resolves, excluding branch-only consumers while the change is proposed.
- `satisfied` or `waived`: resolved, and the consumer's span still matches the resolution anchor.
- `stale`: resolved, but the span has changed since the resolution.
- `proposed`: every diff file is at its base hash.
- `diverged`: some diff file is at neither its base nor its new hash, or the diff files are mixed.
- `revisited`: every diff file is at its new hash, and the consumer's span differs from its anchor.
- `open`: every diff file is at its new hash, and the span matches its anchor.

It SHALL report `pending` while a relevant file is ahead of the served snapshot. The engine SHALL NOT infer that an obligation is satisfied.

#### Scenario: The lifecycle of one obligation
- **WHEN** an obligation is recorded, the exact previewed diff is applied, the consumer's own lines are edited, and then the consumer is deleted
- **THEN** its state reads `proposed`, `open`, `revisited` and `gone` in turn

#### Scenario: A different change lands
- **WHEN** an obligation is recorded and the agent then applies a different edit to the same file
- **THEN** it reads `diverged`

#### Scenario: A consumer that exists only on the branch
- **WHEN** the diff adds a new caller that the preview records as a consumer, and the diff has not been applied
- **THEN** that obligation reads `proposed`, not `gone`

### Requirement: Only the host resolves an obligation
The daemon SHALL expose `obligation_resolve` (MCP `graph_obligation_resolve`), which accepts an obligation id, a status of `satisfied` or `waived`, and a note. It SHALL store the resolution, including the consumer's span hash at resolution time, by rewriting the sidecar atomically.

#### Scenario: A resolution goes stale when the code moves on
- **WHEN** an obligation is resolved `satisfied` and the consumer's lines later change
- **THEN** it reads `stale`

### Requirement: Obligations are inert to analysis and retrieval
Obligation nodes SHALL follow the existing rules for `memory:` nodes. They get no centrality or community, and are excluded from `graph.json`. They SHALL never appear in `query`, `context`, `impact` or `path` results. A preview SHALL never record an obligation about another obligation.

#### Scenario: Obligations never enter retrieval
- **WHEN** `context` or `impact` runs on a consumer that has open obligations
- **THEN** no `memory:obligation:` node appears in the result
