## MODIFIED Requirements

### Requirement: Endpoint resolution runs in every build path and is tallied
`resolve_contracts` SHALL run after `resolve_raw_relations` in both `run_one_shot` and the incremental `rebuild_graph`, so an edit to a chain's prefix, mounts or client calls re-paths its endpoints on the next update; `resolve_raw_relations` SHALL ignore `route`, `file_route`, `mounts`, `aliases`, `http_call` and `http_wrapper` facts; `stats.json` SHALL carry `route_resolution` with `routes`, `routes_unresolved`, `mounts`, `mounts_unresolved`, `endpoints`, `calls`, `calls_unresolved`, `consumes` and `endpoints_external`; and `endpoint` nodes SHALL never enter fuzzy dedup, so sibling routes with near-identical labels stay distinct.

#### Scenario: Sibling endpoints survive dedup
- **GIVEN** endpoints `GET /api/v1/notebooks/:id/notes`, `GET /api/v1/notebooks/:id/votes` and `GET /api/v1/notebooks/:id/note` in one community and one file
- **WHEN** `semantic_dedup` runs
- **THEN** all three nodes remain

#### Scenario: The tally is reported
- **WHEN** a one-shot build writes `stats.json`
- **THEN** it contains `route_resolution` with the nine counts, all zero for a repository without routers or HTTP client calls
