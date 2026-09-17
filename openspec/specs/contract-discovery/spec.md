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

