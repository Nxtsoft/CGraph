## ADDED Requirements

### Requirement: A LangGraph config serves the Agent Server routes
A file named `langgraph.json` whose `graphs` member is a non-empty object SHALL yield a `langgraph_server` node spanning that object, one `langgraph_graph` node per entry (with `entrypoint` when the entry names one), and a served `endpoint:<METHOD> <path>` node `handled_by` the server for every route of the LangGraph Agent Server surface (the 49 routes `@langchain/langgraph-api` 1.5.1 registers and implements; its four `crons` routes answer 500 "Not implemented" and are not served). A route group the config's `http.disable_<group>` flag switches off SHALL NOT be served. Unless the config sets `node_version` (the JS server does not read it; a warning says so), `http.mount_prefix` SHALL be normalized as langgraph-api 0.15.1 does: an empty or null value, or `/`, serves at the root; one trailing `/` is dropped; the result prefixes every served route, with `GET /ok` also served at the root. A value the server rejects (not a string, no leading `/`, `/noauth` or beneath it, or still ending in `/` after the drop) SHALL be a warning and no served routes, as the server refuses to start. A `langgraph.json` without a non-empty `graphs` object SHALL yield its file node only.

#### Scenario: A client of the Agent Server meets its provider
- **GIVEN** a repository with `langgraph.json` naming one graph and a file calling `fetch(\`${url}/runs/wait\`, { method: 'POST' })`
- **THEN** `endpoint:POST /runs/wait` is served, `handled_by` the `langgraph_server` node, and consumed by the calling function

#### Scenario: A disabled group is not served
- **GIVEN** `"http": {"disable_store": true}`
- **THEN** no `/store/...` endpoint is emitted, and the runs routes are

#### Scenario: A mount prefix moves the surface
- **GIVEN** `"http": {"mount_prefix": "/my-deployment/api"}` and no `node_version`
- **THEN** `endpoint:POST /my-deployment/api/runs/wait` is served, `POST /runs/wait` is not, and `GET /ok` is served at both spellings

#### Scenario: A mount prefix is normalized as the server does
- **GIVEN** `"http": {"mount_prefix": "/api/"}`
- **THEN** `endpoint:POST /api/runs/wait` is served; with `""` or `"/"` the routes stay at the root

#### Scenario: A malformed mount prefix is reported
- **GIVEN** `"http": {"mount_prefix": "api"}`, `"//"`, `"/noauth"` or a number
- **THEN** a warning names the invalid prefix and no route is served

#### Scenario: A config without graphs declares no server
- **GIVEN** a `langgraph.json` with no `graphs`, or an empty one
- **THEN** only its file node is emitted
