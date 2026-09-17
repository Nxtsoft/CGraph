# contract-discovery Specification

## Purpose
TBD - created by archiving change discover-route-endpoints. Update Purpose after archive.
## Requirements
### Requirement: Route registrations become endpoint nodes
For every inline HTTP route handler the JavaScript/TypeScript extractor names (`<chain>.<verb>('<path>', ..., handler)` with `verb` in get/post/put/patch/delete/head/options/all), the pipeline SHALL mint one `endpoint` node per (method, full path) with id `endpoint:<METHOD> <canonical path>` where the canonical path replaces every parameter segment (`:id`, `{id}`, `[id]`) with `{}` and keeps `*` and literal segments, label `<METHOD> <full path>` in the provider's spelling, kind `endpoint`, properties `method` and `path` (provider's spelling), the handler's `source_file` and `source_location`, a `contains` edge from the handler's file node, and a `handled_by` edge from the endpoint to the handler; two handlers registering the same method and canonical path SHALL share one endpoint node with two `handled_by` edges.

#### Scenario: A handler mints its endpoint
- **GIVEN** `const app = new Elysia(); app.get('/health', () => 'ok')` in `src/app.ts`
- **WHEN** the graph is built
- **THEN** a node `endpoint:GET /health` exists with kind `endpoint`, `method` `GET`, `path` `/health`, source file `src/app.ts`, a `contains` edge from the file and a `handled_by` edge to the `app.get /health` handler

#### Scenario: A parameterised route has a canonical id
- **GIVEN** `new Elysia({ prefix: '/api/v1' }).get('/notebooks/:id/notes', h)`
- **THEN** the node's id is `endpoint:GET /api/v1/notebooks/{}/notes`, its label `GET /api/v1/notebooks/:id/notes` and its `path` `/api/v1/notebooks/:id/notes`

#### Scenario: A repository without routers is unchanged
- **GIVEN** a repository whose files register no routes, mount no chains and make no HTTP client calls
- **THEN** the graph gains no node, edge or property, and `route_resolution` reports zero routes, mounts, calls and endpoints

### Requirement: Full paths compose through router mounts
The full path of a route SHALL be its chain's own prefix (`new Elysia({ prefix })` option or `.basePath()` link, recorded as the chain variable's `route_prefix` property) joined beneath every path of every chain that mounts it (`.use(child)`, `.use('/p', child)`, `.route('/p', child)`, read through `as`, `satisfies`, `!`, parenthesis and `<T>` casts), recursively to the top-level chains, with mount targets resolved through the mounting file's imports (by alias when imported `as` one) and then its own declarations to a `variable` node; a variable whose value is another identifier (`export const deckModule = deckRoutes as unknown as Elysia`) SHALL compose as a mount with no path and no edge; a chain built inline as the argument of an enclosing chain's `.use()` or `.route('/p', …)`, and the parameter of a `.group('/p', app => …)` or `.guard(opts, app => …)` callback, SHALL root at the enclosing chain with the mount or group path and its own constructor prefix beneath it, and its handlers SHALL be labelled by that chain and composed path; a chain nobody mounts SHALL be top-level; a chain mounted under several paths SHALL mint one endpoint per path; a mount cycle SHALL stop at the revisited chain; paths SHALL join with single slashes, duplicate slashes collapsed and no trailing slash except the root; each resolved mount SHALL become a `mounts` edge from the mounting to the mounted variable carrying a `prefix` property when the mount has a path.

#### Scenario: Three import hops and two casts compose one path
- **GIVEN** `notebookRoutes = new Elysia({ prefix: '/notebooks' }).get('/starred-notes', handler)` in one file, `notebooksModule = new Elysia().use(notebookRoutes) as unknown as Elysia` importing it in a second, and `apiRoutes = new Elysia({ prefix: '/api/v1' }).use(notebooksModule as any)` with `app = new Elysia().use(apiRoutes as any)` importing it in a third
- **THEN** the endpoint is `GET /api/v1/notebooks/starred-notes`, and `mounts` edges run app → apiRoutes → notebooksModule → notebookRoutes

#### Scenario: Express mount paths and a doubly mounted router
- **GIVEN** `router.get('/items', handler)` in one file and `app.use('/api', router); app.use('/legacy', router)` importing it
- **THEN** endpoints `GET /api/items` and `GET /legacy/items` both have `handled_by` edges to the one handler, and the app → router `mounts` edge carries `prefix` `/api`

#### Scenario: A root path under a prefix
- **GIVEN** a chain with prefix `/notebooks` registering `.get('/')`
- **THEN** the endpoint path is `/notebooks`

#### Scenario: Aliased import, inline chain, group callback and cast alias
- **GIVEN** `apiRoutes = new Elysia({ prefix: '/api/v1' }).use(configModule).use(deckModule).use(new Elysia({ prefix: '/inline' }).get('/protected', h)).group('/v2', (app) => app.get('/x', h2))` with `import { config as configModule }` of `config = new Elysia({ prefix: '/config' }).get('/luna', h3)` and `deckModule = deckRoutes as unknown as Elysia` over `deckRoutes = new Elysia({ prefix: '/notebook-docs' }).post('/:id/deck', h4)`
- **THEN** the endpoints are `GET /api/v1/config/luna`, `POST /api/v1/notebook-docs/:id/deck`, `GET /api/v1/inline/protected` and `GET /api/v1/v2/x`, the inline and grouped handlers are labelled `apiRoutes.get /inline/protected` and `apiRoutes.get /v2/x`, and there is no `mounts` edge from `deckModule` to `deckRoutes`

### Requirement: Unresolvable chains are refused and counted
A route whose chain the extractor could not root (a router received as an ordinary function parameter, an unassigned chain in an expression statement) or whose chain identifier resolves to no `variable` node through the file's imports or declarations SHALL mint no endpoint and SHALL be counted in `route_resolution.routes_unresolved`; a mount whose target identifier resolves to nothing SHALL be counted in `mounts_unresolved`; a mount whose target is not an identifier (`.use(cors())`) or resolves to a non-variable (middleware function) SHALL be neither a mount nor an error.

#### Scenario: Parameter and local routers are refused
- **GIVEN** `export const app = new Elysia({ prefix: '/real' }).get('/y', h)`, `export function register(app) { app.get('/x', h2) }` and `describe('r', () => { const app = new Elysia(); app.post('/probe', h3) })` in one file
- **THEN** `GET /real/y` is minted, no endpoint ending in `/x` or `/probe` exists, and `routes_unresolved` is 2

### Requirement: Next.js route files become endpoints
A module-level function named by an uppercase HTTP verb (`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`) in a `route.ts|js|tsx|jsx|mjs` file under an `app` directory SHALL mint an endpoint whose path is the file's path below the last `app` directory with `[name]` segments as `:name`, `[...name]` and `[[...name]]` as `*`, and `(group)` and `@slot` segments dropped; a lowercase `get` in such a file SHALL NOT.

#### Scenario: A dynamic segment route file
- **GIVEN** `export async function GET()` and `export const POST = async () => {}` in `app/api/admin/connectors/[id]/route.ts`
- **THEN** endpoints `GET /api/admin/connectors/:id` and `POST /api/admin/connectors/:id` exist, each `handled_by` its function

### Requirement: HTTP client calls become CONSUMES edges to endpoints
For every call to `fetch`, to `<receiver>.<verb>` where the receiver's name contains api, client, axios, ky, got, http, fetch, request or agent and no argument is a function, or to a function that resolves (through the file's imports, then its own declarations) to a wrapper whose own such call appends its first parameter to a fixed prefix, the pipeline SHALL reduce the URL argument to a path (literal text kept; a leading interpolation dropped as the host; a whole-segment interpolation `{}`; module-level string constants inlined; `+` concatenation read as a template; query string and fragment cut), compose wrapper prefix and argument, canonicalise it, take the method from the call's literal `method` option, else the wrapper's own literal method, else the client verb, else GET, and add a `CONSUMES` edge from the calling function (else the module-level variable the call initialises, else the file) to `endpoint:<METHOD> <canonical path>`, minting that node with `served: false`, no source and the canonical label when no route in the repository serves it; a URL held in a variable, an absolute `http(s)://` literal, or a path not starting with `/` SHALL add no edge and SHALL be counted in `route_resolution.calls_unresolved`; a call through a name that is no wrapper SHALL be neither an edge nor a count.

#### Scenario: A path wrapper's callers are the consumers
- **GIVEN** `const base = \`${API_URL}/api/v1\``, `async function apiFetch(path, options) { return fetch(\`${base}${path}\`, options) }` and `export const notebooksApi = { list: () => apiFetch('/notebooks'), get: (id) => apiFetch(\`/notebooks/${id}\`), create: (i) => apiFetch('/notebooks', { method: 'POST' }) }` in one file that serves no routes
- **THEN** `notebooksApi` has `CONSUMES` edges to `endpoint:GET /api/v1/notebooks`, `endpoint:GET /api/v1/notebooks/{}` and `endpoint:POST /api/v1/notebooks`, each with `served` `false` and no source file, and `apiFetch` itself has none

#### Scenario: A direct fetch and openapi-fetch
- **GIVEN** `fetch(\`${API_BASE_URL}/api/v1/projects/${projectId}/publish\`, { method: 'POST' })` in `publishProject` and `api.GET('/api/v1/projects/{id}', …)` in `loadProject`
- **THEN** `publishProject` consumes `endpoint:POST /api/v1/projects/{}/publish` and `loadProject` consumes `endpoint:GET /api/v1/projects/{}`

#### Scenario: A consumer of the repository's own route joins the served node
- **GIVEN** `export async function GET()` in `app/api/auth/token/route.ts` and `fetch('/api/auth/token')` in `token()`
- **THEN** one node `endpoint:GET /api/auth/token` has a `handled_by` edge to `GET`, a `CONSUMES` edge from `token`, and no `served` property

#### Scenario: Refusals and non-requests
- **GIVEN** `const url = build(); fetch(url)`, `fetch('https://api.github.com/repos/x/y')` and `cache.get('/api/v1/key')`
- **THEN** no endpoint for them exists, `calls_unresolved` counts the first two, and the Map lookup is counted nowhere

### Requirement: Contract documents declare endpoints and schemas
The pipeline SHALL extract an OpenAPI JSON document (a `.json` file named with `openapi` or `swagger`; OpenAPI 3 or Swagger 2), a Protocol Buffers file (`.proto`) and a GraphQL SDL file (`.graphql`, `.gql`, `.graphqls`) into its `file` node plus: for OpenAPI, one `endpoint` per `paths.<path>.<method>` (id `endpoint:<METHOD> <canonical path>`, label and `path` in the document's spelling, `documented: true`, `operation` from `operationId`) and one `schema` per component schema (Swagger 2: definition) with a `field` per property (`type_text` from `type`, `format`, `$ref` and array `items`; `optional` false when listed in `required`), with `RESPONDS_WITH` for each 2xx or default response schema `$ref`, `ACCEPTS` for each request-body (Swagger 2: body parameter) schema `$ref`, `references` for property `$ref`s, `inherits` for `allOf` `$ref`s; for Protocol Buffers, one `schema` per `message` (nested as `Outer.Inner`) and `enum` with a `field` per member (labels `repeated`/`optional`/`required` kept in `type_text`, `map<K,V>` as written, `oneof` members flattened, enum values as fields), one `type` per `service` and one `endpoint` per `rpc` at `POST /<package>.<Service>/<Method>` with `ACCEPTS` its request and `RESPONDS_WITH` its response message, `defines` from the service, and `references` from a message to the messages its fields name, resolved within the file only; for GraphQL, one `schema` per type, interface, input, enum, union and scalar with a `field` per member (`type_text` as written, `optional` false when non-null), `implements` for interface clauses, `references` for field types and union members, and for each field of a root operation type (`Query`, `Mutation`, `Subscription`, or the `schema {}` block's) one `endpoint` `<OPERATION> <field>` with `RESPONDS_WITH` its unwrapped return type and `ACCEPTS` each argument type, `defines` from the root type, and no `field` for it; every emitted node SHALL have a `contains` edge from the file node; a `.json` without an `openapi` or `swagger` key SHALL yield a warning and no nodes; and a TypeScript file declaring both `export interface paths` and `export interface operations` SHALL be read as openapi-typescript output: `paths` members become documented endpoints (a method typed `never` is absent), `components.schemas` members become `schema` nodes with fields, `operations` request bodies and 2xx responses naming `components["schemas"]["X"]` become `ACCEPTS` and `RESPONDS_WITH`, and the `paths`, `operations` and `components` interfaces yield no `field` nodes.

#### Scenario: An OpenAPI document's paths and schemas
- **GIVEN** `openapi.json` with `/api/v1/notebooks` offering `get` (200 → array of `#/components/schemas/Notebook`) and `post` (requestBody `CreateNotebook`, 201 → `Notebook`), `/api/v1/notebooks/{id}/notes` with `get`, `delete` and a `parameters` key, an `x-internal` path, and schemas `Notebook` (properties id/title/owner→`User`/tags, required id and title), `CreateNotebook`, `User`, `AdminUser` (`allOf` `User`)
- **THEN** four endpoints exist, `GET /api/v1/notebooks/{id}/notes` has id `endpoint:GET /api/v1/notebooks/{}/notes` and `documented` `true`, `Notebook` has fields id/owner/tags/title with `tags` `type_text` `string[]` and `id` `optional` `false`, the list endpoint `RESPONDS_WITH` `Notebook`, the create endpoint `ACCEPTS` `CreateNotebook`, `Notebook` `references` `User`, and `AdminUser` `inherits` `User`

#### Scenario: A proto service
- **GIVEN** `package notes.v1;`, messages `Notebook` (with nested `Owner`, a `oneof`, a `map<string, Tag>` and `repeated Note notes`), `Note`, `Tag`, enum `Kind`, and `service Notebooks { rpc List (ListRequest) returns (ListResponse); rpc Watch (ListRequest) returns (stream Notebook) { option … } }`
- **THEN** schemas `Notebook`, `Notebook.Owner`, `Note`, `Tag`, `Kind` and type `Notebooks` exist, `Notebook`'s fields include the `oneof` members and not `Owner`'s, endpoints `POST /notes.v1.Notebooks/List` and `POST /notes.v1.Notebooks/Watch` exist with `defines` from the service, `List` `ACCEPTS` `ListRequest` and `RESPONDS_WITH` `ListResponse`, `Watch` `RESPONDS_WITH` `Notebook`, and `Notebook` `references` `Note`, `Tag` and `Notebook.Owner`

#### Scenario: A GraphQL schema
- **GIVEN** `type Notebook implements Node & Timestamped @key(...) { id: ID! owner: User notes(first: Int = 10): [Note!]! }`, `extend type Notebook { archived: Boolean }`, `input CreateNotebookInput`, `union SearchResult = Notebook | Note`, `type Query { notebooks(kind: Kind): [Notebook!]! }`, `type Mutation { createNotebook(input: CreateNotebookInput!): Notebook! @auth }` and a `schema { query: Query mutation: Mutation }` block
- **THEN** `Notebook`'s fields are archived/id/kind/notes/owner/title, `notes` has `type_text` `[Note!]!` and `optional` `false`, `Notebook` `implements` `Node` and `references` `User`, endpoints `QUERY notebooks` and `MUTATION createNotebook` exist with `defines` from `Query` and `Mutation`, `notebooks` `RESPONDS_WITH` `Notebook` and `ACCEPTS` `Kind`, `createNotebook` `ACCEPTS` `CreateNotebookInput`, `Query` has no fields, and `String`/`ID` are referenced by nothing

#### Scenario: openapi-typescript output
- **GIVEN** a `.d.ts` with `export interface paths { "/api/v1/notebooks": { get: operations["listNotebooks"]; put?: never; post: operations["createNotebook"] } }`, `export interface components { schemas: { Notebook: { id: string; owner?: components["schemas"]["User"] }; User: { id: string } } }` and `export interface operations` whose `listNotebooks` 200 response is `components["schemas"]["Notebook"][]`
- **THEN** endpoints `GET /api/v1/notebooks` and `POST /api/v1/notebooks` exist with `documented` `true` and `operation` ids, no `field` named `/api/v1/notebooks` exists, schema `Notebook` has fields `id` and `owner` (optional) and `references` `User`, and the list endpoint `RESPONDS_WITH` `Notebook`

### Requirement: A documented endpoint is the node a route serves or a client consumes
`resolve_contracts` SHALL attach `handled_by` and `CONSUMES` edges to an existing documented endpoint node of the same canonical id instead of minting a second node, SHALL leave its document `source_file` and `documented` property in place, SHALL NOT mark it `served: false`, and SHALL count documented endpoints in `route_resolution.endpoints_documented`.

#### Scenario: Served and consumed documented endpoints
- **GIVEN** an openapi-typescript file documenting `GET /api/auth/token` and `GET /api/v1/notebooks`, a Next.js route file exporting `GET` for `/api/auth/token`, and `fetch('/api/v1/notebooks')` in `listNotebooks`
- **THEN** exactly two endpoint nodes exist, the token endpoint has `handled_by` to `GET` and its source file is the document, the notebooks endpoint has `CONSUMES` from `listNotebooks` and no `served` property, and `endpoints_documented` is 2 while `endpoints` and `endpoints_external` are 0

