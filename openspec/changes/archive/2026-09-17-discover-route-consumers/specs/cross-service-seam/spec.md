## ADDED Requirements

### Requirement: Seam discovery emits the contract fragment from discovered contracts
The engine SHALL provide `cgraph seam discover --graph NAME=graph.json [--graph …] --out DROPDIR`, which reads each named graph's `endpoint` nodes, `handled_by` edges and `CONSUMES` edges and writes a `chunk_00.json` fragment with no spec: a `service` node `service:<NAME>` per graph (properties `role` `discovered`, `graph`), every endpoint some graph serves or consumes (a served copy's label and `path` winning over a consumer's `served: false` placeholder), `SERVED_BY` (endpoint → service) and `HANDLED_BY` (endpoint → the handler's `code-ref` shadow) for each serving graph, `CONSUMES` (service → endpoint) and `CONSUMED_AT` (endpoint → the caller's `code-ref` shadow) for each consuming graph; an endpoint neither served nor consumed SHALL be omitted; the fragment SHALL satisfy `validate_semantic_fragment_json`, fuse with `seam fuse`, and be byte-equivalent on regeneration; the log SHALL name per service what it serves and consumes, the endpoints matched across services, those consumed with no provider among the graphs, and those served with no consumer; an unreadable graph SHALL be a hard error emitting no fragment.

#### Scenario: Two graphs join on a canonical id
- **GIVEN** an api graph with `endpoint:GET /api/v1/notebooks/starred-notes` handled by `api::handler`, and a web graph where `web::useStarred` `CONSUMES` the same id and also `endpoint:POST /api/v1/orphan`
- **WHEN** `seam discover` runs over both
- **THEN** the fragment has `service:api`, `service:web`, the starred-notes endpoint without `served`, edges `SERVED_BY` → `service:api`, `HANDLED_BY` → `api::handler`, `CONSUMES` from `service:web`, `CONSUMED_AT` → `web::useStarred`, the orphan endpoint with `served` `false`, and the log says `matched 1 endpoints`

#### Scenario: A missing graph is refused
- **WHEN** a `--graph` path does not exist
- **THEN** the command fails with an error naming the graph and writes no fragment
