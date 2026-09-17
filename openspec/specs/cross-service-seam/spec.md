# cross-service-seam Specification

## Purpose
Deterministic generation of a cross-service contract fragment from a host-authored seam spec and the
consumer code graphs, joining services by their wire contracts (endpoints, schemas) with cross-graph
edges anchored to real nodes (never dangling), emitted as a standard ingestable fragment.
## Requirements
### Requirement: Seam generation emits a cross-service contract fragment
The engine SHALL provide deterministic generation of a cross-service contract fragment from a
host-authored seam spec and one or more named consumer code graphs, exposed as the
`cgraph seam gen --seam SPEC --graphs NAME=graph.json[ --graphs …] --out DROPDIR` subcommand. The
output SHALL be a single node-link fragment (`chunk_00.json`) in the drop directory whose nodes use
the kinds `service`, `endpoint`, `schema`, and `code-ref`, with stable ids `service:<name>`,
`endpoint:<provider>:<METHOD> <path>`, `schema:<provider>:<api_version>:<name>`, and (for a
`code-ref`) the resolved consumer node's own id; and whose edges use the relations `CONSUMES`
(service→endpoint), `SERVED_BY` (endpoint→provider service), `RESPONDS_WITH` (endpoint→schema),
`CONSUMED_AT` (endpoint→consumer code node), and `MIRRORED_BY` (schema→consumer code node). Nodes
SHALL be deduplicated by id. Regenerating from the same spec and graphs SHALL produce a
byte-equivalent fragment.

#### Scenario: Spec and graphs produce the contract fragment
- **WHEN** `cgraph seam gen` runs on a spec declaring a provider, services, schemas, endpoints,
  `consumes`, and `mirrors`, with the named consumer graphs supplied
- **THEN** the emitted fragment contains a `service` node per service, an `endpoint` node per
  endpoint with `SERVED_BY` → its provider service and `RESPONDS_WITH` → its response schema, a
  `schema` node per schema, a `CONSUMES` edge from each consuming service to its endpoint, and a
  `code-ref` shadow node plus `CONSUMED_AT` / `MIRRORED_BY` edge for each resolved anchor

#### Scenario: Regeneration is byte-stable
- **WHEN** the same spec and graphs are passed to `cgraph seam gen` twice
- **THEN** the two emitted fragments are byte-equivalent

### Requirement: Cross-graph anchors resolve to real nodes or fail loud
Each `consumes.call_site` and each `mirror` SHALL be resolved against the named consumer graph to the
smallest non-`file` node whose `source_file` matches the anchor path and whose `source_location`
span contains the anchor line; the resolved node's real id SHALL be used as the `code-ref` target so
no cross-graph edge is dangling. If any anchor resolves to no node, the command SHALL fail and emit
no fragment — a partial or dangling seam SHALL NOT be produced.

#### Scenario: Anchor resolves to the smallest containing node
- **WHEN** an anchor line falls within several nested nodes' spans in the consumer graph
- **THEN** the anchor resolves to the node with the smallest span, and the `CONSUMED_AT` /
  `MIRRORED_BY` edge targets that node's real id

#### Scenario: Unresolvable anchor is a hard error
- **WHEN** an anchor's `(file, line)` matches no node span in the named consumer graph
- **THEN** the command fails with a precise error naming the anchor and writes no fragment

### Requirement: Seam spec and references are validated before emission
The seam spec SHALL be validated before any fragment is emitted: required fields present, every
`consumes` entry referencing a declared endpoint, every `mirror` referencing a declared schema, and
every anchor's graph name matching a supplied `--graphs` entry. Any violation SHALL be a hard error
that emits no fragment. The emitted fragment SHALL satisfy `validate_semantic_fragment_json`, so it
is ingestable through the existing fragment-ingest path without any validation change.

#### Scenario: Unknown endpoint or schema reference is rejected
- **WHEN** a `consumes` entry names a `method`+`path` with no matching declared endpoint, or a
  `mirror` names a schema not in `schemas`
- **THEN** the command fails with a precise error and writes no fragment

#### Scenario: Missing consumer graph is rejected
- **WHEN** a `call_site` or `mirror` names a graph for which no `--graphs NAME=…` was supplied
- **THEN** the command fails with a precise error and writes no fragment

#### Scenario: Emitted fragment is ingestable
- **WHEN** a seam fragment is generated successfully
- **THEN** it passes `validate_semantic_fragment_json` and can be ingested via `cgraph enrich-ingest`
  without modification

### Requirement: Seam fuse renders a clustered multi-service view
The engine SHALL provide a view-only fused render of a seam, exposed as the
`cgraph seam fuse --seam SEAM --graph NAME=graph.json[ --graph …] --out DIR` subcommand. It SHALL
merge the seam fragment with the named consumer code graphs into a single node-link graph and write
`graph.json` and `graph.html` to the output directory. Every node from a `--graph NAME=…` service
SHALL be tagged `properties.community = NAME`, and each seam `service` / `endpoint` / `schema` node
SHALL be tagged with its community (the service name for a `service` node, the provider for an
`endpoint` or `schema` node), so the existing renderer (which clusters and colors by
`properties.community`) draws each service as its own cluster joined by the seam's contract edges.
Edges SHALL be deduplicated. The fused output is a static artifact — it is NOT a daemon and NOT
queryable.

#### Scenario: Each service renders as its own cluster
- **WHEN** `cgraph seam fuse` runs on a seam fragment plus the supplied service graphs
- **THEN** the written `graph.json` tags every node with a `properties.community` (the service name,
  or the provider for endpoint/schema nodes), and `graph.html` is produced for opening

### Requirement: Shadow code-refs collapse onto real service nodes
When fusing, the seam's `code-ref` shadow nodes SHALL be dropped, because the real service node
carrying the same id is already present from that service's graph (with its full neighborhood and
real source location). The seam's `CONSUMED_AT` and `MIRRORED_BY` edges SHALL therefore attach to
the real service nodes, so the contract is drillable into the surrounding code.

#### Scenario: Contract edge binds to the real node, not a shadow
- **WHEN** a seam fragment contains a `code-ref` shadow for a consumer call site and the consumer's
  service graph is supplied to fuse
- **THEN** the fused graph contains no `code-ref` node for that id, and the `CONSUMED_AT` edge
  targets the real service node already present from the service graph

### Requirement: Fuse fails loud on a missing service graph
Every endpoint of every fused edge SHALL resolve to a node present in the fused node set. If any
edge endpoint id is absent — because the owning service graph was not supplied via `--graph` — the
command SHALL fail with a precise error and write no fused graph; a dangling render SHALL NOT be
produced.

#### Scenario: Omitting a referenced service graph is rejected
- **WHEN** a seam edge references a node from a service graph that was not passed to `cgraph seam
  fuse`
- **THEN** the command fails with an error identifying the missing endpoint and writes no output

### Requirement: A fused seam graph is queryable read-only
The engine SHALL expose `cgraph seam query --graph FUSED.json <op> [PARAMS_JSON]`, which loads a
fused seam graph and runs one read op against it, printing the op's JSON response. The supported ops
are `query`, `path`, `explain`, `impact`, and `context` — the daemon's read ops — dispatched over the
loaded snapshot so they answer cross-service questions (traversing `CONSUMES`, `SERVED_BY`,
`RESPONDS_WITH`, `CONSUMED_AT`, `MIRRORED_BY` alongside each service's own edges). A non-read op
SHALL be rejected with a clear message and a non-zero exit; the seam graph is a read-only derived
view. Each invocation loads the graph fresh (one-shot); the seam is a static snapshot refreshed by
re-running `seam fuse`.

#### Scenario: Cross-service impact over the seam
- **WHEN** `cgraph seam query --graph FUSED.json impact '{"id":"schema:<provider>:<v>:<Name>","direction":"dependents"}'`
  runs on a fused seam graph
- **THEN** the response lists the dependents reached across services (e.g. the endpoint that
  responds with that schema and the consuming service), drawn from the contract edges

#### Scenario: Cross-service path over the seam
- **WHEN** `cgraph seam query --graph FUSED.json path '{"source":"<consumer node id>","target":"endpoint:<provider>:<METHOD> <path>"}'`
  runs
- **THEN** the response returns a path connecting the consumer code node to the provider endpoint
  across the contract edge (or reports no path), never erroring on the cross-service boundary

#### Scenario: Write ops are rejected
- **WHEN** `cgraph seam query` is invoked with an op outside `query`/`path`/`explain`/`impact`/`context`
  (e.g. `remember` or `update`)
- **THEN** the command rejects it with a clear message and a non-zero exit, writing no changes

### Requirement: A fused seam is served by a resident read-only daemon
`cgraph seam fuse` SHALL write a `.cgraph-seam` marker into its output directory alongside
`graph.json`. When `graphd` is started on a directory containing that marker, it SHALL run a static
seam serve loop rather than the build-and-watch server: it SHALL load the directory's `graph.json`,
publish it as the snapshot, and serve the read ops (`query`, `path`, `explain`, `impact`, `context`,
`status`) via the existing request handler, WITHOUT building a graph, watching files, persisting, or
running enrichment. Write ops SHALL be rejected (the seam is a read-only derived view); `update`
SHALL reload `graph.json` from disk; `shutdown` SHALL stop the daemon. The seam daemon SHALL be
addressed by the same per-root identity as any project, so the existing client and MCP tools reach it
by pointing their root / `project_root` at the seam directory, with no client or MCP change.

#### Scenario: A seam directory is served statically and queried cross-service
- **WHEN** a client (or MCP tool) addresses a seam directory (one produced by `seam fuse`, carrying
  the marker) as its root and issues `impact` / `explain` / `path` / `context`
- **THEN** `graphd` serves it from the loaded fused graph without building or watching, and returns
  cross-service answers (traversing the contract edges)

#### Scenario: Repeated queries are served by the resident daemon
- **WHEN** a second read op is issued against the same seam directory while the daemon is up
- **THEN** it is served by the resident daemon without reloading the graph, and `status` reports the
  loaded node/edge counts

#### Scenario: Writes are rejected; update reloads
- **WHEN** a write op (e.g. `remember` or fragment ingest) is sent to a seam daemon
- **THEN** it is rejected; and an `update` reloads `graph.json` from disk so a re-`fuse` is picked up
  without a rebuild

#### Scenario: A normal project is unaffected
- **WHEN** `graphd` starts on a directory without the `.cgraph-seam` marker
- **THEN** it runs the normal build-and-watch server unchanged

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

