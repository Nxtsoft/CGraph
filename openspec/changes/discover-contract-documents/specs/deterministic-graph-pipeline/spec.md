## MODIFIED Requirements

### Requirement: Endpoint resolution runs in every build path and is tallied
`resolve_contracts` SHALL run after `resolve_raw_relations` in both `run_one_shot` and the incremental `rebuild_graph`, so an edit to a chain's prefix, mounts or client calls re-paths its endpoints on the next update; `resolve_raw_relations` SHALL ignore `route`, `file_route`, `mounts`, `aliases`, `http_call`, `http_wrapper` and `url_const` facts; `stats.json` SHALL carry `route_resolution` with `routes`, `routes_unresolved`, `mounts`, `mounts_unresolved`, `endpoints`, `calls`, `calls_unresolved`, `consumes`, `endpoints_external` and `endpoints_documented`; `endpoint` and `schema` nodes SHALL never enter fuzzy dedup, so sibling routes and sibling schemas with near-identical labels stay distinct; and `report types` SHALL treat `schema` nodes as type owners alongside `class` and `type`.

#### Scenario: Sibling endpoints and schemas survive dedup
- **GIVEN** endpoints `GET /api/v1/notebooks/:id/notes`, `GET /api/v1/notebooks/:id/votes` and `GET /api/v1/notebooks/:id/note`, and schemas `NotebookResponse`, `NotebooksResponse` and `NotebookResponses`, in one community and one file
- **WHEN** `semantic_dedup` runs
- **THEN** all six nodes remain

#### Scenario: A schema and its mirror in the types view
- **GIVEN** schema `Notebook` in `openapi.json` with fields id/title/owner and type `Notebook` in `types.ts` with the same fields, plus schema `NoteDto` and type `Note` with identical member sets
- **THEN** the types report counts four types, lists `Notebook` as one duplicate row at Jaccard 1.0, and groups `NoteDto` and `Note` as an identical shape

#### Scenario: The tally is reported
- **WHEN** a one-shot build writes `stats.json`
- **THEN** it contains `route_resolution` with the ten counts, all zero for a repository without routers, HTTP client calls or contract documents

### Requirement: Contract documents are detected
`detect_language` SHALL classify `.proto` as `Protobuf`, `.graphql`, `.gql` and `.graphqls` as `GraphQL`, and a `.json` file whose name contains `openapi` or `swagger` (case-insensitive) as `OpenApi`; every other `.json` and every `.yaml` / `.yml` SHALL stay `Unknown`; and the three languages SHALL have registered non-grammar extractors so they never appear in `unextracted`.

#### Scenario: Detection by name and extension
- **WHEN** `openapi.json`, `docs/Swagger.v2.JSON`, `petstore.openapi.json`, `openapi.yaml`, `package.json`, `api/notes.proto`, `schema.graphql`, `schema.gql` and `schema.graphqls` are detected
- **THEN** the first three are `OpenApi`, `openapi.yaml` and `package.json` are `Unknown`, the proto is `Protobuf` and the last three are `GraphQL`
