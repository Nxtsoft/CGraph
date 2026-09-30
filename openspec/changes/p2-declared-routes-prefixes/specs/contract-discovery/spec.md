## ADDED Requirements

### Requirement: A LangGraph config serves the Agent Server routes
A file named `langgraph.json` whose `graphs` member is a non-empty object SHALL yield a `langgraph_server` node spanning that object, one `langgraph_graph` node per entry (with `entrypoint` when the entry names one), and a served `endpoint:<METHOD> <path>` node `handled_by` the server for every route of the LangGraph Agent Server surface (the 49 routes `@langchain/langgraph-api` 1.5.1 registers and implements; its four `crons` routes answer 500 "Not implemented" and are not served). A route group the config's `http.disable_<group>` flag switches off SHALL NOT be served. A `langgraph.json` without a non-empty `graphs` object SHALL yield its file node only.

#### Scenario: A client of the Agent Server meets its provider
- **GIVEN** a repository with `langgraph.json` naming one graph and a file calling `fetch(\`${url}/runs/wait\`, { method: 'POST' })`
- **THEN** `endpoint:POST /runs/wait` is served, `handled_by` the `langgraph_server` node, and consumed by the calling function

#### Scenario: A disabled group is not served
- **GIVEN** `"http": {"disable_store": true}`
- **THEN** no `/store/...` endpoint is emitted, and the runs routes are

#### Scenario: A config without graphs declares no server
- **GIVEN** a `langgraph.json` with no `graphs`, or an empty one
- **THEN** only its file node is emitted
