## MODIFIED Requirements

### Requirement: Seam discovery emits the contract fragment from discovered contracts
The engine SHALL provide `cgraph seam discover --graph NAME=graph.json [--graph …] --out DROPDIR`, which reads each named graph's `endpoint` nodes, `handled_by` edges, `CONSUMES` edges and `documented` properties and writes a `chunk_00.json` fragment with no spec: a `service` node `service:<NAME>` per graph (properties `role` `discovered`, `graph`), every endpoint some graph serves, consumes or documents (a served or documented copy's label and `path` winning over a consumer's `served: false` placeholder), `SERVED_BY` (endpoint → service) and `HANDLED_BY` (endpoint → the handler's `code-ref` shadow) for each serving graph, `CONSUMES` (service → endpoint) and `CONSUMED_AT` (endpoint → the caller's `code-ref` shadow) for each consuming graph, and `DOCUMENTED_IN` (endpoint → the document's `file` `code-ref` shadow) for each documenting graph; an endpoint neither served, consumed nor documented SHALL be omitted; the fragment SHALL satisfy `validate_semantic_fragment_json`, fuse with `seam fuse`, and be byte-equivalent on regeneration; the log SHALL name per service what it serves, consumes and documents, the endpoints matched across services, those consumed with no provider among the graphs, those served with no consumer, and, when any graph documents endpoints, a `drift:` line counting endpoints documented but served by no service among the graphs, served but in no document, and only documented; an unreadable graph SHALL be a hard error emitting no fragment.

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
