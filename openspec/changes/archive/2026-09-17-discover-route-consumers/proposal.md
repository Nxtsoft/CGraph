# Feature: consumers of endpoints, and `seam discover` (CGR-13, slice 2)

## Why

Slice 1 (#97) put every route a service serves into its graph as an `endpoint` node. The other
half of a contract is who calls it. turing-webapp calls turing-api through three shapes, none
of which names the endpoint in one place: 326 direct `fetch(\`${API_URL}/api/v1/projects/${id}\`)`
calls where the host is an interpolation and the path a literal with parameter segments; 151
calls to small path wrappers (`apiFetch(path)` over `fetch(\`${base}${path}\`)` with
`const base = \`${API_URL}/api/v1\``, and `categoryFetch`, `orgFetch`, `post`, `patch`, `del`
of the same shape); and 39 openapi-fetch calls `api.GET('/api/v1/projects/{id}', …)`. Eighteen
`fetch('/api/…')` calls hit the webapp's own Next.js route files. The plan's field test wrote one
consumer by hand and covered 0 of 32 typed call sites automatically; the CGR-13 acceptance is
`seam discover` emitting every route and consumer of the pair with no spec file.

## What Changes

- **Canonical endpoint ids.** Every parameter segment is `{}` in the id (`:id`, `{id}`, `[id]`,
  a template `${id}`): `endpoint:GET /api/v1/notebooks/{}/notes`. The label and `path` property
  keep the provider's spelling (`GET /api/v1/notebooks/:id/notes`). A consumer in another repo
  cannot know the provider's parameter names, and a workspace daemon (CGR-14) forwards by id, so
  the id must not carry them. `canonical_route_path` is the one definition.
- **Consumer facts at extraction** (`javascript_extractor.cpp`). `http_call`: a call to `fetch`,
  to `<client>.<verb>` where the receiver's name says it is an HTTP client (`api`, `axios`, `ky`,
  `http`, `client`, …) and no argument is a function (a handler argument is a route
  registration, MSW's `http.get('/x', handler)` included), or to any function with a path-like
  literal first argument; source = the enclosing function, else the module-level variable the
  call initialises (`export const notebooksApi = { list: () => apiFetch('/notebooks') }`), else
  the file; context = the literal `method` of an options object and the URL reduced to a path:
  literal text kept, a leading interpolation dropped as the host, a whole-segment interpolation
  `{}`, module-level string constants inlined (`base`), `+` concatenation read as a template,
  query and fragment cut, an absolute `https://…` literal or a URL held in a local variable
  recorded with an empty path (refused, counted). `http_wrapper`: a function whose own client
  call appends its first parameter to that reduced prefix (`apiFetch(path)`: prefix `/api/v1`;
  `del(path)` with `method: 'DELETE'` in its own call: fixed method).
- **Consumer resolution** (`contracts.cpp`, after routes). A call through a wrapper name
  resolves the name through imports then same-file declarations to a function with a wrapper
  fact; a name that is no wrapper is an ordinary function taking a string and is skipped
  untallied. The path is prefix + argument, canonicalised; the method is the call's literal,
  else the wrapper's fixed one, else the client verb, else GET. The caller gets a `CONSUMES`
  edge to the endpoint, which is minted with `served: false` and no source when this repo does
  not serve it; a route resolved later for the same id takes the node over (served spelling,
  handler anchor, `contains`, no `served`). `route_resolution` gains `calls`, `calls_unresolved`,
  `consumes`, `endpoints_external`.
- **`cgraph seam discover --graph NAME=graph.json [--graph …] --out DROPDIR`** (`seam.cpp`,
  `discover_seam`). Reads the contracts each graph carries and writes the same fragment shape
  `seam gen` does, with no spec: a `service` node per graph, every served or consumed endpoint,
  `SERVED_BY` and `HANDLED_BY` (endpoint → handler code-ref) for providers, `CONSUMES` and
  `CONSUMED_AT` (endpoint → caller code-ref) for consumers; a served copy of an endpoint wins
  over a consumer's placeholder. The log names per service what it serves and consumes, how
  many endpoints matched across services, and how many are consumed with no provider among
  the given graphs. `SeamGraph` now reads `links` and string properties. `seam fuse` renders the
  result unchanged.
- Docs: README endpoints section, skill (consumer questions route to `graph_impact` on the
  endpoint; cross-repo via `seam discover` + `fuse`), host contract, CLAUDE.md.

### Non-goals
- Wrappers of wrappers (`function get(path) { return apiFetch(path) }`): the wrapper's own call
  must be to a primitive client.
- A wrapper whose path parameter is not its first parameter.
- Consumers whose URL is assembled in a variable (`const url = …; fetch(url)`): refused and
  counted as `calls_unresolved`, 105 of the webapp's 452 direct `fetch` calls.
- Calls to another host spelled as a literal absolute URL: another service's contract, refused.
- Schema files (OpenAPI, protobuf, GraphQL): slice 3. Repo-scoped ids and the workspace daemon:
  CGR-14.

## Impact

- **Touches:** `contracts.hpp/.cpp`, `javascript_extractor.cpp`, `graph_builder.cpp` (skip list),
  `operation_stats.hpp/.cpp`, `seam.hpp/.cpp`, `src/cli/main.cpp`, `contracts_test.cpp`,
  `javascript_extractor_test.cpp`, `seam_test.cpp`, docs.
- **Ids change** for parameterised endpoints minted by slice 1 (`:id` → `{}` in the id only).
  Slice 1 merged the same day and nothing persisted them; labels, paths and every other node id
  are unchanged. A repo with no HTTP client calls and no routes gains nothing.
- **Measured** on the field-test pair (tasks.md 3.3-3.5).

## Capabilities

### Modified Capabilities
- `contract-discovery` — endpoint ids are canonical; client calls become `CONSUMES` edges and mint unserved endpoints.
- `cross-service-seam` — `seam discover` writes the seam fragment from discovered contracts, with `HANDLED_BY` in the vocabulary.
- `deterministic-graph-pipeline` — `route_resolution` reports the consumer tally.
