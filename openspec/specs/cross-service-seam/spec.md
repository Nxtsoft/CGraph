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

### Requirement: Seam discovery and fuse join a consumer through its declared proxy prefix
`seam discover` and `seam fuse` SHALL accept `--prefix REPO:/from=/to`. With a prefix, an endpoint that graph `REPO` consumes but neither serves nor documents, whose path is `from` or lies beneath it on a segment boundary, SHALL be joined as the same method at `to` + the rest, unless only `REPO` itself serves that proxied endpoint. The joined endpoint's `CONSUMED_AT` edges SHALL carry `via` = the consumer's own path, and the resolution log SHALL count the endpoints each prefix joined. `seam fuse` SHALL redirect such a consumer's `CONSUMES` edge to the joined endpoint, SHALL NOT render the placeholder it leaves unused, and SHALL fail when the joined endpoint is in neither the seam nor a service graph. A prefix naming a repository no `--graph` provides SHALL be refused (exit 2). Without a prefix nothing is joined across spellings, and no graph's own ids change.

#### Scenario: A proxied call meets the backend
- **GIVEN** `web` consuming `endpoint:PATCH /api/backend/v1/sessions/{}/extend` and `idp` serving `endpoint:PATCH /api/v1/sessions/{}/extend`
- **WHEN** `seam discover` runs with `--prefix web:/api/backend=/api`
- **THEN** `web`'s caller is `CONSUMED_AT` idp's endpoint with `via: /api/backend/v1/sessions/{}/extend`, and the log reads `prefix web /api/backend -> /api: 1 consumed endpoints joined at the proxied path`

#### Scenario: A route the consumer serves itself is not proxied
- **GIVEN** `web` also serves `GET /api/backend/healthz`, and serves `GET /api/saml/metadata`
- **THEN** its call to `/api/backend/healthz` stays on its own endpoint, and its call to `/api/backend/saml/metadata` is not joined to `/api/saml/metadata`

#### Scenario: Fuse refuses an unjoined proxied edge
- **GIVEN** a seam discovered without the provider graph
- **WHEN** fused with the same prefix
- **THEN** fuse fails and names the proxied endpoint

### Requirement: Seam discovery and fuse join every bridged contract, and declared databases and env
`seam discover` SHALL join every contract that crosses repositories (`crossing_id`) the way it joins endpoints, keeping the contract's kind: `SERVED_BY`, `HANDLED_BY`, `CONSUMES` and `CONSUMED_AT`, `served: false` while no graph provides it. A `table:local:` or `label:local:` contract SHALL be left out unless its graph's repo is declared in a database; `--database NAME=repoA,repoB` (and a workspace manifest's `databases`) SHALL spell such a repo's `table:local:<name>` as `table:<NAME>:<name>`, so members of one database meet and no other repo does. An env id SHALL join only when declared: `--env VAR=service` (and a manifest's `env`) SHALL make `env:VAR` a shared contract served by that service; an undeclared env id and a standard HTTP header SHALL be left out. The endpoint log lines SHALL be unchanged; other contracts SHALL be logged on their own line when there are any, and each declared database with the count of repo-local ids it joined. `seam fuse` SHALL share every crossing id across services, scope every other id (`table:local:`, an undeclared `env:` and a standard header included) to its service, with `--database` rename a declared member's repo-local table to the database's id and set its `database` property to that database, and with `--env` share the declared env id. `seam fuse` SHALL be given the declarations discover was: a seam contract other than an endpoint whose `HANDLED_BY` or `CONSUMED_AT` service holds it under none of the ids the given declarations spell it as SHALL fail loud, naming the contract and the missing `--env` or `--database`, rather than splitting the join. A declaration naming a repo that is not among the `--graph`s, a repo in two databases, or a name declared twice SHALL exit 2; both commands SHALL accept both flags.

#### Scenario: A header joins without a declaration
- **GIVEN** api's graph with `header:x-tenant-id` `handled_by` `readTenant` (labelled `X-Tenant-Id`) and ml's with `sendTenant` consuming `header:x-tenant-id`
- **THEN** discover emits `header:x-tenant-id` (kind `header`, label `X-Tenant-Id`, not `served: false`) `HANDLED_BY` api's `readTenant`, `CONSUMED_AT` ml's `sendTenant`, `SERVED_BY` `service:api`; fuse keeps the one shared id

#### Scenario: Tables join only inside a declared database
- **GIVEN** api providing and ml and billing using `table:local:users`
- **WHEN** discover runs with no declaration
- **THEN** no `table:` node is in the seam, and fuse keeps `api::table:local:users` and `ml::table:local:users`
- **WHEN** discover and fuse run with `--database turing=api,ml`
- **THEN** `table:turing:users` is `HANDLED_BY` api's `createUsers` and `CONSUMED_AT` ml's `listUsers`, never billing's; the log says `database turing (api,ml): 2 repo-local tables and labels joined at the database's id`; fuse gives ml's `listUsers` a `CONSUMES` edge to `table:turing:users` while billing keeps `billing::table:local:users`

#### Scenario: A declared env variable is served by its service
- **GIVEN** ml using `env:API_URL` and `--env API_URL=api`
- **THEN** `env:API_URL` is `SERVED_BY` `service:api` and not `served: false`, and fuse with `--env` gives ml's `sendTenant` a `CONSUMES` edge to the shared `env:API_URL`

#### Scenario: Standard headers and undeclared env names stay in their service
- **GIVEN** api and ml both reading `env:NODE_ENV` and using `header:authorization`, and ml using `env:API_URL`, with no `--env`
- **THEN** discover emits no `env:` node and no `header:authorization`, and fuse keeps `api::env:NODE_ENV`, `ml::env:NODE_ENV`, `ml::env:API_URL` and `ml::header:authorization` apart

#### Scenario: Fuse without discover's declarations is refused
- **GIVEN** a seam discovered with `--database turing=api,ml --env API_URL=api`
- **WHEN** fuse runs without `--env`, or without `--database`
- **THEN** it fails with an error naming `env:API_URL` and `--env`, or `table:turing:users` and `--database`

### Requirement: Seam discovery and fuse join claims within a declared issuer
`seam discover` and `seam fuse` SHALL accept `--issuer NAME=repoA,repoB` (a workspace manifest's `issuers`), validated as `--database` is: a malformed value, the name `local`, a repo that is not among the `--graph`s, a repo in two issuers, or a name declared twice SHALL exit 2. Discover SHALL join a member's claim at the issuer's id `crossing_id` gives (`claim:<NAME>:<name>`), with the node's `issuer` property set to the issuer and its label the claim's own name, and SHALL leave out a member's RFC 7519 registered claims; a repo outside the issuer SHALL keep the undeclared rule and never join the members' claims. Discover SHALL log `issuer NAME (repoA,repoB): N claims joined at the issuer's id` for each declared issuer and no such line when none is declared, so that with no `--issuer` the seam and its log are unchanged. Fuse SHALL rename a member's claim to the issuer's id with the `issuer` property and keep every other claim's existing treatment; fused with `--issuer` declarations other than discover's (fewer or more), a seam claim SHALL fail loud naming the contract and saying discover and fuse were given different `--issuer` declarations.

#### Scenario: Members of an issuer join, outsiders do not
- **GIVEN** idp providing `claim:session_id`, `claim:email`, `claim:sub` and `claim:roles`; web using `claim:session_id`, `claim:email` and `claim:sub`; cli using `claim:email`; api using `claim:session_id`, `claim:roles` and `claim:email`
- **WHEN** discover runs with no declaration
- **THEN** `claim:session_id` is `HANDLED_BY` idp's `mintToken` and `CONSUMED_AT` api's `checkToken`, and no `claim:email` node is in the seam
- **WHEN** discover runs with `--issuer idp=idp,web,cli`
- **THEN** `claim:idp:email` (kind `claim`, label `email`, `issuer: idp`) is `HANDLED_BY` idp's `mintToken` and `CONSUMED_AT` web's `readToken` and cli's `whoami`; `claim:idp:session_id` is `CONSUMED_AT` web's `readToken` and not api's; no `claim:sub`, `claim:idp:sub` or `claim:email` node exists; api's `claim:session_id` has no provider and no `issuer`; the log says `issuer idp (idp,web,cli): 6 claims joined at the issuer's id`

#### Scenario: Fuse shares the issuer's ids and refuses without them
- **GIVEN** that seam
- **WHEN** fuse runs without `--issuer`
- **THEN** it fails naming `--issuer`
- **WHEN** a seam discovered with no `--issuer` is fused with `--issuer idp=idp,web,cli`
- **THEN** it fails naming `claim:session_id` and saying discover and fuse were given different `--issuer` declarations
- **WHEN** fuse runs with `--issuer idp=idp,web,cli`
- **THEN** web's `readToken` and cli's `whoami` have `CONSUMES` edges to `claim:idp:email` (`issuer: idp`), `claim:idp:email` is `handled_by` idp's `mintToken`, api's `checkToken` consumes `claim:session_id` and `api::claim:email`, and `web::claim:sub` stays scoped

