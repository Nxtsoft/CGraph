## MODIFIED Requirements

### Requirement: Shadow code-refs collapse onto real service nodes
When fusing, every node id from a supplied service graph SHALL be scoped as `<service>::<id>`, except `endpoint:`, `service:` and `schema:` ids, which SHALL stay shared so a provider and its consumers meet at one node. The seam's `code-ref` shadow nodes SHALL be dropped, and every seam edge into service code SHALL carry a `service` property (written by `seam discover` and `seam generate`) and SHALL be rewritten to that service's scoped ids; a seam edge that names a `code-ref` shadow but carries no `service` SHALL fail the fuse with an error asking to regenerate the seam, and no fused graph SHALL be written. Thus the `CONSUMED_AT` and `MIRRORED_BY` edges attach to the real service nodes (with their full neighborhood and real source location) and the contract is drillable into the surrounding code. Two services that own a node with the same project-relative id SHALL fuse into two nodes, each keeping only its own service's edges.

#### Scenario: Contract edge binds to the real node, not a shadow
- **WHEN** a seam fragment contains a `code-ref` shadow for a consumer call site and the consumer's service graph is supplied to fuse
- **THEN** the fused graph contains no `code-ref` node, and the `CONSUMED_AT` edge targets the consumer service's scoped node for that call site

#### Scenario: Two services with the same relative file stay apart
- **GIVEN** services `api` and `agents` whose graphs both contain `src_db_client_ts`, each imported by its own file, and both consuming `endpoint:GET /x`
- **WHEN** they are fused
- **THEN** the fused graph holds `api::src_db_client_ts` and `agents::src_db_client_ts` with their own source files, no import edge crosses between them, and both importers consume the one `endpoint:GET /x`

#### Scenario: A seam edge from an older seam is refused
- **GIVEN** a seam fragment whose `CONSUMED_AT` edge targets a `code-ref` shadow and carries no `service` property
- **WHEN** it is fused
- **THEN** fuse fails with an error naming the edge and asking to regenerate the seam, and writes no fused graph

### Requirement: Seam discovery emits the contract fragment from discovered contracts
The engine SHALL provide `cgraph seam discover --graph NAME=graph.json [--graph …] --out DROPDIR`, which reads each named graph's `endpoint` nodes, `handled_by` edges, `CONSUMES` edges and `documented` properties and writes a `chunk_00.json` fragment with no spec: a `service` node `service:<NAME>` per graph (properties `role` `discovered`, `graph`), every endpoint some graph serves, consumes or documents (a served or documented copy's label and `path` winning over a consumer's `served: false` placeholder), `SERVED_BY` (endpoint → service) and `HANDLED_BY` (endpoint → the handler's `code-ref` shadow) for each serving graph, `CONSUMES` (service → endpoint) and `CONSUMED_AT` (endpoint → the caller's `code-ref` shadow) for each consuming graph, and `DOCUMENTED_IN` (endpoint → the document's `file` `code-ref` shadow) for each documenting graph; each `HANDLED_BY`, `CONSUMED_AT` and `DOCUMENTED_IN` edge SHALL carry the property `service` naming the graph whose code it points into, and two graphs whose code shares a raw id SHALL each keep their own such edge; an endpoint neither served, consumed nor documented SHALL be omitted; the fragment SHALL satisfy `validate_semantic_fragment_json`, fuse with `seam fuse`, and be byte-equivalent on regeneration; the log SHALL name per service what it serves, consumes and documents, the endpoints matched across services, those consumed with no provider among the graphs, those served with no consumer, and, when any graph documents endpoints, a `drift:` line counting endpoints documented but served by no service among the graphs, served but in no document, and only documented; an unreadable graph SHALL be a hard error emitting no fragment.

#### Scenario: Two graphs join on a canonical id
- **GIVEN** an api graph with `endpoint:GET /api/v1/notebooks/starred-notes` handled by `api::handler`, and a web graph where `web::useStarred` `CONSUMES` the same id and also `endpoint:POST /api/v1/orphan`
- **WHEN** `seam discover` runs over both
- **THEN** the fragment has `service:api`, `service:web`, the starred-notes endpoint without `served`, edges `SERVED_BY` → `service:api`, `HANDLED_BY` → `api::handler`, `CONSUMES` from `service:web`, `CONSUMED_AT` → `web::useStarred`, the orphan endpoint with `served` `false`, and the log says `matched 1 endpoints`

#### Scenario: A document graph reports drift
- **GIVEN** the two graphs above and a third whose `openapi.json` file node documents `GET /api/v1/notebooks/starred-notes` and `GET /api/v1/removed`, while the api graph also serves `GET /api/v1/health`
- **THEN** both documented endpoints carry `DOCUMENTED_IN` → the document's shadow, `GET /api/v1/removed` is kept without `served`, the log has `service spec: serves 0 endpoints, consumes 0, documents 2` and `drift: 1 documented but served by no service here, 1 served but in no document; 1 only documented (neither served nor consumed)`

#### Scenario: A missing graph is refused
- **WHEN** a `--graph` path does not exist
- **THEN** the command fails with an error naming the graph and writes no fragment

#### Scenario: Two services whose callers share a raw id keep both edges
- **GIVEN** graphs `web` and `worker` whose files both have id `src_api_ts` and both consume `endpoint:GET /api/v1/x`
- **WHEN** `seam discover` runs over both
- **THEN** the fragment holds two `CONSUMED_AT` edges from that endpoint to `src_api_ts`, one with `service` `web` and one with `service` `worker`

