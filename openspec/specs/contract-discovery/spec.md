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
For every call to `fetch`, to `<receiver>.<verb>` where the receiver's name contains api, client, axios, ky, got, http, fetch, request or agent and no argument is a function, or to a function that resolves (through the file's imports, then its own declarations) to a wrapper whose own such call appends one of its parameters to a fixed prefix, the pipeline SHALL reduce the URL argument to a path (literal text kept; a leading interpolation dropped as the host, except a call (`${base(id)}/x`) to a builder the file does not define, which leaves the request unresolved because the builder may hold part of the path, while a module function of the same file whose one return value reads as a path is inlined with its parameters as segment values; a whole-segment interpolation `{}`; module-level string constants inlined; a class field `this.X` set once to a string inlined, and one whose value is a path the file cannot read (`X || '/api/roles'`) making the request unresolvable; a block-scoped local standing for the whole URL read through its value when that value reads as a path and the local is set once (an initializer never reassigned, or assignments before the call in opposite branches of one if/else, giving one consumer per branch; a write in the other branch of an if/else from the read never reaches it unless a loop around the read carries it back), and a local reassigned (including by destructuring, `({ url } = cfg)` or `[url] = cfg`), built from itself, bound by a loop header or `catch`, assigned in another function or updated in place being unknowable; `new URL(x)`, `u.toString()` and `u.href` read as `x`; a local or same-file helper holding only a query string starting the query; `+` concatenation read as a template; query string and fragment cut), join an axios instance's `baseURL` (from `axios.create({ baseURL })` in a module constant or a `this.x` member) to the path the way axios does, read calls to a class method (`this.patch(...)`) or module function of the same file through the one shape its client calls agree on, compose wrapper prefix and argument, canonicalise it, take the method from the call's literal `method` option (one consumer per verb when it chooses between two verb literals) where the wrapper lets it (a wrapper's options object read member by member, a later member winning, so `{ method: 'GET', ...init }` takes the call's method and `{ ...options, method: 'PATCH' }` does not; options held in a local set once, a spread object literal, and the rest of a destructured options parameter read as their value; options whose method the file cannot read, being a value it cannot see, something other than a parameter spread in after any literal method, or a method neither a literal nor a parameter, leaving unresolved a direct client call, a `this.x(...)` call into a same-class wrapper, and a call forwarding a parameter into a same-file wrapper), else the wrapper's own literal method, else the client verb, else GET, and add a `CONSUMES` edge from the calling function (else the module-level variable the call initialises, else the file) to `endpoint:<METHOD> <canonical path>`, minting that node with `served: false`, no source and the canonical label when no route in the repository serves it. A wrapper whose path is a later parameter or whose method is a parameter SHALL be filled from the call's own arguments (a verb literal for the method, the parameter's literal default when the call leaves it out); a call whose method is not spelled out, or whose options there the file cannot read, SHALL be counted unresolved. The `http_wrapper` fact SHALL record which parameter's options the wrapper spreads into its own (`{ ...init }`), whether the wrapper's own method is unreadable, and both verbs of a method choice. A call to any wrapper SHALL take its method from the call's options at that recorded index, whatever its other arguments are (`apiFetch(path, authHeaders, { method: 'POST' })` is POST), else from the wrapper's own method (one consumer per verb of a choice), else GET; a wrapper that spreads no caller options SHALL send its own method whatever a caller passes. A call whose options there the file cannot read, or that leaves to the wrapper a method the wrapper's file cannot read, SHALL be counted unresolved, never guessed GET. A path without a leading slash SHALL be joined only to a prefix ending in `/`. A URL held in a variable whose value cannot be read or led by a call it cannot read, an absolute `http(s)://` literal, or a path not starting with `/` that nothing joins SHALL add no edge and SHALL be counted in `route_resolution.calls_unresolved`; a call through a name that is no wrapper SHALL be neither an edge nor a count.

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

#### Scenario: A call at the front of a URL is not a host
- **GIVEN** `const base = (id) => \`${BACKEND_URL}/api/v1/projects/${id}\`` and `fetch(\`${base(projectId)}/oracles\`, { method: 'POST' })` in `viaHelper`, `fetch(\`${importedBase(projectId)}/runs\`, { method: 'POST' })` in `viaImportedHelper`, and `fetch(\`${config.baseUrl}/items\`)` in `viaMember`
- **THEN** `viaHelper` consumes `endpoint:POST /api/v1/projects/{}/oracles`, no `endpoint:POST /runs` exists and that call is counted in `calls_unresolved`, while `viaMember` consumes `endpoint:GET /items`

#### Scenario: A typed awaited request is a request
- **GIVEN** `r = await axios.post<{ ok: boolean }>(\`${BACKEND_URL}/api/v1/assigned-generic\`, {})` and `const s = await axios.post<LoginResponse>(\`${BACKEND_URL}/api/v1/login\`, {})` in `typedAwait`, where `BACKEND_URL` is built from `process.env`
- **THEN** `typedAwait` consumes `endpoint:POST /api/v1/assigned-generic` and `endpoint:POST /api/v1/login`

#### Scenario: A URL held in a local
- **GIVEN** `const backendUrl = \`${BACKEND_URL}/api/v1/invitations/${token}/mfa/verify\`; fetch(backendUrl, { method: 'POST' })`, `const u = new URL(\`${BACKEND_URL}/api/v1/skills/catalog\`); fetch(u.toString())`, and `let e; if (t) e = new URL(\`${BACKEND_URL}/api/v1/auth-events/recent/realm/${t}\`); else e = new URL(\`${BACKEND_URL}/api/v1/auth-events/recent\`); fetch(e.toString())`
- **THEN** the caller consumes `POST /api/v1/invitations/{}/mfa/verify`, `GET /api/v1/skills/catalog`, `GET /api/v1/auth-events/recent/realm/{}` and `GET /api/v1/auth-events/recent`, while a URL whose `pathname` is reassigned is counted unresolved

#### Scenario: A local that is a host or a value keeps its reading
- **GIVEN** `const backendUrl = 'http://localhost:8080'` read as `request.get(\`${backendUrl}/api/v1/users?t=1\`)`, and `let sessionId` assigned `'test-session-id'` in a `beforeAll` callback read as `axios.patch(\`${API}/api/v1/sessions/${sessionId}/extend\`)`
- **THEN** the calls consume `GET /api/v1/users` and `PATCH /api/v1/sessions/{}/extend`, and no endpoint names `test-session-id`

#### Scenario: Class wrappers and axios base URLs
- **GIVEN** a class with `this.api = axios.create({ baseURL: '/api/backend' })`, `patch(url) { return this.api.patch(url) }`, `enableUser(id) { return this.patch(\`v1/users/${id}/enable\`) }`, `this.api.get(\`v1/public/tenants/${t}/theme/css\`)`, and a field `API_BASE = '/api/backend'` read as `axios.post(\`${this.API_BASE}/v1/passkeys/register/start\`)`
- **THEN** `enableUser` consumes `PATCH /api/backend/v1/users/{}/enable`, the theme call `GET /api/backend/v1/public/tenants/{}/theme/css`, and the passkey call `POST /api/backend/v1/passkeys/register/start`

#### Scenario: A wrapper whose path and method are later parameters
- **GIVEN** `async function mlRequest(baseUrl, method, path) { return fetch(\`${baseUrl}${path}\`, { method }) }`, `export function mlBackendRequest(method, path, projectId) { return mlRequest(mlBackendBaseUrl, method, path) }` and `presenceFetch(path, method = 'GET')` fetching `\`/api/v1/notes${path}\`` with `{ method }`, called from another file as `mlBackendRequest('POST', \`/project/${id}/setup\`, id)`, `mlBackendRequest(verb, '/simulate/runs', id)`, `presenceFetch(\`/${id}/presence\`)` and `presenceFetch(\`/${id}/presence\`, 'POST')`
- **THEN** the callers consume `POST /project/{}/setup`, `GET /api/v1/notes/{}/presence` and `POST /api/v1/notes/{}/presence`, and the call with the unknown `verb` mints nothing and is counted in `calls_unresolved`

#### Scenario: A relative path joins only a base ending in a slash
- **GIVEN** `apiRequest(endpoint, options)` fetching `\`${API_BASE}/${endpoint.replace(/^\//, '')}\`` with `{ ...options }` and `apiFetch(path)` fetching `\`${API_BASE}${path}\`` with `API_BASE = '/api/backend'`, called as `apiRequest(\`v1/service-providers/${id}/refresh-metadata\`, { method: 'POST' })` and `apiFetch(\`v1/users/${id}\`)`
- **THEN** the first consumes `POST /api/backend/v1/service-providers/{}/refresh-metadata` and the second mints nothing and is counted in `calls_unresolved`

#### Scenario: A wrapper's method follows spread order
- **GIVEN** `request(path, init) { return fetch(\`${API}${path}\`, { method: 'GET', ...init }) }` called as `this.request('/api/v1/widgets', { method: 'POST' })` and as `this.request('/api/v1/widgets')`, and `authenticatedPatch(url, body, options)` calling `authenticatedFetch(url, { ...options, method: 'PATCH' })`, called as `authenticatedPatch(\`/api/backend/v1/tenants/${id}/settings/backup-codes\`, settings)`
- **THEN** the callers consume `POST /api/v1/widgets`, `GET /api/v1/widgets` and `PATCH /api/backend/v1/tenants/{}/settings/backup-codes`

#### Scenario: A wrapper's method is read, not guessed
- **GIVEN** `request(path, init) { const opts = { ...init, method: 'DELETE' }; return fetch(\`${API}${path}\`, opts) }` called as `this.request('/api/v3/widgets', { method: 'POST' })`; `request(path, init) { return fetch(\`${API}${path}\`, { method: 'GET', ...init }) }` called as `const opts = { method: 'POST' }; this.request('/api/v1/varopts', opts)`, as `this.request('/api/v1/tern', { method: on ? 'PUT' : 'DELETE' })` and as `this.request('/api/v1/unknown', buildOptions())`; `{ method: 'GET', ...init, ...{ method: 'HEAD' } }` called with `{ method: 'POST' }`; and `{ method: 'GET', ...defaults }` with `defaults` imported
- **THEN** the callers consume `DELETE /api/v3/widgets`, `POST /api/v1/varopts`, `PUT /api/v1/tern`, `DELETE /api/v1/tern` and `HEAD /api/v4/widgets`, and the calls through `buildOptions()` and `...defaults` mint nothing and are counted in `calls_unresolved`

#### Scenario: A positional wrapper's options are read or counted
- **GIVEN** `authed(headers, path, init)` fetching `\`/api/v1${path}\`` with `init`, called from another file as `authed(h, '/absent')`, `authed(h, '/no-method', { headers: h })`, `authed(h, '/literal', { method: 'POST' })`, `const opts = { method: 'PATCH' }; authed(h, '/via-local', opts)`, `authed(h, '/unreadable', buildOptions())` and `authed(h, '/unreadable-method', { method: pick() })`
- **THEN** the callers consume `GET /api/v1/absent`, `GET /api/v1/no-method`, `POST /api/v1/literal` and `PATCH /api/v1/via-local`, and the two unreadable calls mint nothing and are counted in `calls_unresolved`

#### Scenario: A write in the other branch does not reach the read
- **GIVEN** `let url; if (admin) { url = \`${API}/api/b/admin\` } else { url = \`${API}/api/b/user\`; return fetch(url) }`, an else-if chain reading in its last branch, and `let url = \`${API}/api/l/destr\`; ({ url } = cfg); fetch(url)`
- **THEN** the first caller consumes only `GET /api/b/user`, the chain only its last branch's URL, and the destructured read is counted in `calls_unresolved`

#### Scenario: A local that is reassigned is not read
- **GIVEN** `let url = \`${API}/api/v1/things\`; if (tag) url = url + '/tagged'; fetch(url)`, `let url = \`${API}/api/v1/old\`; url = \`${API}/api/v1/new\`; fetch(url)`, and `const url = \`${API}/api/v1/outer\`; for (const url of urls) fetch(url)`
- **THEN** no endpoint `/tagged`, `/api/v1/old`, `/api/v1/new` or `/api/v1/outer` exists and each call is counted in `calls_unresolved`

#### Scenario: A wrapper's options are read where it takes them
- **GIVEN** `apiFetch(path, authHeaders, init)` fetching `\`/api/v1${path}\`` with `{ ...init, headers: { ...authHeaders } }`, `plain(path)` fetching with no options, `built(path, init)` fetching with `decorate(init)`, `mixed(path, init)` fetching with `{ method: pick(), ...init }` and `toggle(path, on)` fetching with `{ method: on ? 'POST' : 'DELETE' }`, called from another file as `apiFetch('/inputs', authHeaders)`, `apiFetch('/inputs', authHeaders, { method: 'POST', body })`, `apiFetch(\`/composites/${id}/top-level\`, authHeaders, { method: 'PATCH' })`, `apiFetch('/unreadable', authHeaders, makeInit())`, `plain('/plain', { method: 'POST' })`, `built('/built')`, `built('/spelled', { method: 'PUT' })`, `mixed('/mixed', { method: 'PUT' })`, `mixed('/bare')`, `toggle('/flags', true)`, and `jsonFetch(url, init)` fetching `url` with `{ ...init }` called as `jsonFetch(LINKS_URL, { method: 'POST' })` (a module constant `\`${HOST}/api/v1/links/\``), `jsonFetch(new URL(\`${HOST}/api/v1/sessions\`).toString(), {})` through a local, and `jsonFetch(url)` with a local `url`, and a class whose `raw(path)` fetches with no options called in the class as `this.raw('/saved', { method: 'POST' })`
- **THEN** the callers consume `GET /api/v1/inputs`, `POST /api/v1/inputs`, `PATCH /api/v1/composites/{}/top-level`, `GET /api/v1/plain`, `PUT /api/v1/mixed`, `POST /api/v1/flags`, `DELETE /api/v1/flags`, `POST /api/v1/links`, `GET /api/v1/sessions`, `GET /api/v1/config` and `GET /api/v1/saved`; the `unreadable`, `built`, `spelled` and `bare` calls mint nothing and are counted in `calls_unresolved`

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

### Requirement: Spring request mappings become endpoints
In Kotlin and Java, a method annotated `@GetMapping`, `@PostMapping`, `@PutMapping`, `@DeleteMapping` or `@PatchMapping`, or `@RequestMapping` with a `method` argument, SHALL mint one endpoint per (verb, path). Each endpoint SHALL be `handled_by` the method, and its path SHALL be the nearest enclosing class's `@RequestMapping` path joined with the method's path. A mapping with no path SHALL serve the class path itself. Paths SHALL be read only from string literals, given positionally or as `value =` or `path =`, singly or as an array. Only a method whose nearest enclosing type is a concrete class SHALL mint. A method of an interface (including a `@FeignClient`, which calls the route rather than serving it), an abstract, sealed or enum class (whose mappings Spring serves only through subclasses, under their prefix), an object or companion object, or a top-level function SHALL mint nothing. The first Spring HTTP mapping on a method decides; other `*Mapping` annotations such as `@MessageMapping` SHALL be ignored. Several positional path arguments SHALL each mint. A path that is not exactly one plain string literal (a constant, a template, a concatenation, a raw string), and a method-level `@RequestMapping` without `method`, SHALL mint no endpoint.

#### Scenario: A Kotlin controller under a class prefix
- **GIVEN** `@RequestMapping("/api/v1/users")` on a class whose methods carry `@GetMapping`, `@GetMapping("/{id}", produces = [...])` and `@RequestMapping(value = ["/sync", "/resync"], method = [RequestMethod.PUT])`
- **THEN** endpoints `GET /api/v1/users`, `GET /api/v1/users/{id}`, `PUT /api/v1/users/sync` and `PUT /api/v1/users/resync` exist, each `handled_by` its method, and `GET /api/v1/users/{id}` has id `endpoint:GET /api/v1/users/{}`

#### Scenario: A Java controller with named arguments
- **GIVEN** `@RequestMapping(path = "/api/orders")` on a class with `@GetMapping(value = {"", "/all"})` and `@RequestMapping(value = "/legacy", method = RequestMethod.POST)`
- **THEN** endpoints `GET /api/orders`, `GET /api/orders/all` and `POST /api/orders/legacy` exist

#### Scenario: Unreadable mappings mint nothing
- **GIVEN** `@GetMapping(ApiPaths.EXPORT)` and a method-level `@RequestMapping("/any")` with no `method`
- **THEN** neither mints an endpoint

#### Scenario: A client consumes a Spring endpoint
- **GIVEN** a Kotlin `@GetMapping` serving `/api/v1/users` and `fetch('/api/v1/users')` in a TypeScript function
- **THEN** the TypeScript function has a `CONSUMES` edge to that endpoint

#### Scenario: Interfaces and non-class scopes serve nothing
- **GIVEN** a `@FeignClient` interface and a plain interface whose methods carry `@GetMapping`, a Java interface under `@RequestMapping("/api/v2")`, an abstract base controller in Kotlin and in Java, a sealed class, a `companion object`, a nested `object`, and a top-level function, each with a mapping
- **THEN** none of them mints an endpoint

#### Scenario: Shapes that still mint
- **GIVEN** `@GetMapping("/one", "/two")`, `@MessageMapping("/ws")` above `@GetMapping("/after-message")`, `@GetMapping(value = arrayOf("/arr"))` and a fully qualified `@org.springframework.web.bind.annotation.PostMapping("/fq")` under `@RequestMapping("/v")`, plus a Java record controller
- **THEN** `GET /v/one`, `GET /v/two`, `GET /v/after-message`, `GET /v/arr` and `POST /v/fq` exist, while `@GetMapping("/a" + "/b")` and a raw-string path mint nothing

### Requirement: An ORM model maps the SQL table its migration creates
When a module-level JavaScript or TypeScript variable is initialised by a Drizzle table constructor (`pgTable`, `mysqlTable` or `sqliteTable`, through type wrappers such as `as`) whose first argument is a string literal, `resolve_contracts` SHALL add a `maps_table` edge from that variable to the `sql_table` node of the same name (compared the way node ids are, so case differences do not matter). It SHALL add no edge when no `sql_table` node of that name exists, when the name is not a string literal, or when the constructor is any other call. The fact SHALL NOT be resolved as a type reference.

#### Scenario: A Drizzle model maps its migration's table
- **GIVEN** a migration with `CREATE TABLE "competitors"` and `export const competitors = pgTable('competitors', {...})`
- **THEN** the graph has a `maps_table` edge from the `competitors` variable to `sql_table:competitors`

#### Scenario: Quoted mixed-case names and the other dialects map
- **GIVEN** `CREATE TABLE IF NOT EXISTS "User_Library_Favorites"` with `pgTable("User_Library_Favorites", ...) as unknown as Table`, and `CREATE TABLE "sessions"` with `mysqlTable('sessions', ...)`
- **THEN** both variables map their tables

#### Scenario: Names that are not migrated, not literal, or not Drizzle map nothing
- **GIVEN** `pgTable('not_migrated', ...)`, `pgTable(sessions, ...)` where `sessions` is a constant, and `makeTable('competitors', ...)`
- **THEN** none of them has a `maps_table` edge

### Requirement: Kotlin and Go HTTP client calls become CONSUMES edges
The extractor SHALL record `http_call` facts, resolved by the same rules as JavaScript client calls, for: a Kotlin `get`/`post`/`put`/`patch`/`delete`/`head`/`options` member call with at least one argument on a receiver whose last name ends in `client`, `http` or `api` (method = the member); a Go `http.Get`/`Head`/`Post`/`PostForm`, `http.NewRequest`, `http.NewRequestWithContext`, or any Go call passing a context (`ctx`, `x.ctx`, `x.Context()`, `context.Background()`) directly followed by `http.Method<Verb>` or an uppercase literal verb and then a URL-like argument (a string literal starting with `/` or `http(s)://`, or a non-literal expression). A Go call to `httptest` or with a function-literal argument SHALL record no client call. The URL SHALL be reduced to a path: literal text up to `?` or `#`; a whole-segment value `{}`; one leading value the host for Kotlin calls and net/http entry points, while a leading value before a Go client method's base-relative path SHALL leave the call unresolved, as SHALL a second leading value, a call building the front of the URL, a value inside a segment, and an absolute `http(s)://` literal. A client call whose URL is a parameter of the enclosing named function, including from inside a lambda or function literal in that function but not a parameter the lambda itself declares (nor Kotlin's implicit `it`), optionally after a host and literal prefix, SHALL instead record an `http_wrapper` fact for that function with the call's method and prefix; a call to a bare Kotlin name, or a Go call by its function or field name, with a path-like string argument SHALL record an `http_call` with the first such argument, which becomes a `CONSUMES` edge only when the name binds in that file to a wrapper. A client call with an unresolvable URL, a method held in a variable, a Ktor `client.request` (its verb is set in the builder), or a parameter tail that is not a prefix wrapper SHALL add no edge and SHALL be counted in `route_resolution.calls_unresolved`.

#### Scenario: A Ktor client and its wrapper consume Spring endpoints
- **GIVEN** a Spring `AuthController` (`@RequestMapping("/api/v1/auth")`, `@PostMapping("/login")`) and `SessionController` (`@RequestMapping("/api/v1/sessions")`, `@PatchMapping("/{id}/invalidate")`), and a Kotlin class with `revoke(sessionId)` calling `client.patch("$baseUrl/api/v1/sessions/$sessionId/invalidate") { … }`, `login(id)` calling `postOutcome(path = "/api/v1/auth/login", request = id)`, and `postOutcome(path, request)` calling `client.post("$baseUrl$path") { … }`
- **THEN** `revoke` consumes `endpoint:PATCH /api/v1/sessions/{}/invalidate` and `login` consumes `endpoint:POST /api/v1/auth/login`, both endpoints handled by the controller methods, and `postOutcome` consumes nothing

#### Scenario: A Go client method and its wrapper
- **GIVEN** `func (c *Client) postAuth(ctx context.Context, path string, req any)` calling `c.Do(ctx, http.MethodPost, path, body)`, `Login` calling `c.postAuth(ctx, "/api/v1/auth/login", req)` and `Me` calling `c.Do(ctx, http.MethodGet, "/api/v1/auth/me", nil)`
- **THEN** `Login` consumes `endpoint:POST /api/v1/auth/login` and `Me` consumes `endpoint:GET /api/v1/auth/me`

#### Scenario: Refusals and non-requests
- **GIVEN** Kotlin `client.request("$baseUrl${p.endpoint}") { method = … }`, `client.get("$host$prefix/users")`, `client.get("https://example.com/api/v1/x")`, `cache.get("/api/v1/key")` and `restClient.post().uri("/api/v1/x")`, and Go `s.Client.Mutate(ctx, "POST", n.Base+"/import", body)`, `c.Do(ctx, http.MethodGet, path+"?"+q.Encode(), nil)` and `http.NewRequestWithContext(ctx, method, c.BaseURL+ensureLeadingSlash(path), rdr)`
- **THEN** none of them adds an endpoint, the three Kotlin client calls and the three Go calls are counted in `calls_unresolved`, and the map lookup and the builder chain are counted nowhere

#### Scenario: Go route registrations, test helpers and non-HTTP calls are no requests
- **GIVEN** Go `r.Handle(http.MethodGet, "/api/v1/users", listUsers)`, `c.Method(http.MethodPost, "/api/v1/orders", createOrder)`, `e.Add("DELETE", "/api/v1/items/:id", deleteItem)`, `router.HandlerFunc("PUT", "/api/v1/widgets", putWidget)`, `srv.Route(ctx, http.MethodGet, "/api/v1/hooks", func(w, r) {})`, `httptest.NewRequest(http.MethodGet, "/api/v1/users", nil)`, `httptest.NewRequestWithContext(ctx, …)`, `httpmock.RegisterResponder("GET", "/api/v1/mocked", …)`, `assert.Equal(t, http.MethodPost, r.Method)`, `require.Equal(t, "GET", r.Method)`, `log.Println("GET", "/api/v1/logged")`, `git.Run(ctx, "rev-parse", …)` and `git.Run(ctx, "HEAD", "--quiet")`, beside `s.Client.Mutate(cmd.Context(), "PATCH", "/api/v1/things/"+id+"/enable", nil)`
- **THEN** only `Mutate` is a client call, consuming `endpoint:PATCH /api/v1/things/{}/enable`, and `calls` is 1

#### Scenario: Kotlin receivers that only contain client or http
- **GIVEN** Kotlin `clientRepository.get("/api/v1/looks-like-a-path")`, `httpCache.get("/api/v1/cached")`, `clients.get(key)` inside `find(key)` called as `find("/api/v1/not-an-endpoint")`, beside `ktorClient.get("$baseUrl/api/v1/real")`
- **THEN** only `ktorClient.get` is a client call, consuming `endpoint:GET /api/v1/real`

#### Scenario: A wrapper whose request runs inside a lambda
- **GIVEN** Kotlin `postOutcome(path: String, body: Any) = withContext(Dispatchers.IO) { client.post("$baseUrl$path") { … } }` called as `postOutcome("/api/v1/auth/login", body)`, and Go `func (c *Client) post(ctx, path string, body any) error` returning `retry(func() error { c.Do(ctx, http.MethodPost, path, body) … })` called as `c.post(ctx, "/api/v1/auth/login", nil)`, beside a lambda declaring its own `path` (`ids.map { path -> client.get("$baseUrl$path") }`, `forEach(paths, func(path string) { c.Do(ctx, http.MethodGet, path, nil) })`) in a function called with a path literal
- **THEN** each `login` caller consumes `endpoint:POST /api/v1/auth/login`, and the functions whose lambda declares `path` are no wrappers

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

### Requirement: FastAPI routers become endpoints
In Python, a module-level assignment `x = APIRouter(...)` or `x = FastAPI(...)` SHALL be a `variable` node contained by its file, carrying `route_prefix` when `APIRouter` is given a string-literal `prefix`; a function decorated `@x.<verb>(path, ...)` with `verb` in get/post/put/patch/delete/head/options, or `@x.api_route(path, methods=[...])` (GET when `methods` is absent), SHALL be a route registration on chain `x` per method, handled by the function; `x.include_router(y, prefix="/p")` and `x.mount("/p", y)` SHALL mount `y` under `x` with the literal prefix (a `mount` whose path does not start with `/` or whose app is not a name SHALL NOT be a mount). These compose into full paths exactly as the JavaScript chains of "Full paths compose through router mounts" do, with `y` resolved through the file's imports: an import `as` an alias SHALL bind only the alias, a name a package's `__init__.py` imports SHALL be importable from the package as what it names, a name the package neither declares nor re-exports SHALL be its submodule, and `y` written `module.router` SHALL be the `router` variable of the imported module `module`, and a `y` bound by an enclosing `for y in (a, b):` over a literal tuple or list SHALL be each of its items. A decorated function inside another function, a path or `methods` that is not a list of plain string literals, and an `APIRouter` whose `prefix` is not a string literal SHALL mint no endpoint and SHALL be counted in `routes_unresolved`. An `include_router` inside a function, with a non-literal `prefix`, or on anything but a module-level router of its file (`app = create_app()`, a router imported from another file, `app.router`, `self.app`) SHALL be counted in `mounts_unresolved`, and a router whose every mount is of that kind SHALL mint no endpoint (each route counted in `routes_unresolved`) rather than be served at its own prefix.

#### Scenario: ml-backend's router tree composes
- **GIVEN** `app = FastAPI()` with `app.include_router(project_router)` and `app.include_router(legacy_router)` in `api/app.py`, `router = APIRouter()` with `router.include_router(setup_router)` in `api/routes/project/router.py` importing `from api.routes.project.setup.routes import router as setup_router`, `router = APIRouter(prefix="/project")` with `@router.post("/{project_id}/setup")` on `setup` in that module, and `router.include_router(design_router, prefix="/v1/model")` in `api/legacy/__init__.py` over `@design_router.get("/{model_id}/variables")`
- **THEN** `endpoint:POST /project/{}/setup` (label `POST /project/{project_id}/setup`) is `handled_by` `setup` and `endpoint:GET /v1/model/{}/variables` is `handled_by` its function, each with a `contains` edge from its file

#### Scenario: Empty paths and api_route
- **GIVEN** `router = APIRouter(prefix="/simulate/cluster")` with `@router.get("")` and `@router.api_route("/wake", methods=["POST", "PUT"])`
- **THEN** `GET /simulate/cluster`, `POST /simulate/cluster/wake` and `PUT /simulate/cluster/wake` exist

#### Scenario: Unknowable paths are counted, not guessed
- **GIVEN** a `@inner.get("/inside")` inside `def create_app()`, `elsewhere = APIRouter(prefix=settings.PREFIX)` with a route, `dynamic_router` mounted only by `include_router(dynamic_router, prefix=settings.PREFIX)`, and `@design_router.get(f"/{PREFIX}/computed")`
- **THEN** none of them mints an endpoint (no `GET /dynamic`), `routes_unresolved` counts all four, and `mounts_unresolved` counts the factory mount and the non-literal one

#### Scenario: An alias shadows nothing
- **GIVEN** `pkg/svc.py` with `from pkg.other import router as other_router`, its own `router = APIRouter(prefix="/svc")`, `router.include_router(other_router, prefix="/o")` and `@router.get("/x")`, and `pkg/other.py` with `router = APIRouter(prefix="/other")` and `@router.get("/y")`
- **THEN** exactly `GET /svc/x` and `GET /svc/o/other/y` exist

#### Scenario: Submodules, package re-exports and sub-applications
- **GIVEN** `from app.routers import users` with `app.include_router(users.router, prefix="/api/v1")` over `APIRouter(prefix="/users")` with `@router.get("/{user_id}")`; `api/routes/__init__.py` importing `from api.routes.items import router as items_router` and `main.py` with `from api.routes import items_router` and `app.include_router(items_router, prefix="/v2")` over `APIRouter(prefix="/items")` with `@router.get("/{item_id}")`; and `app.mount("/api", api)` of `api = FastAPI()` with `@api.get("/items")`
- **THEN** `GET /api/v1/users/{}`, `GET /v2/items/{}` and `GET /api/items` exist, and none of them at the router's own top-level path

#### Scenario: A mount nobody can place hides its router
- **GIVEN** `app = create_app()` with `app.include_router(users.router, prefix="/api/v1")`, and `app/wire.py` importing `api_router` from `app/api.py` with `api_router.include_router(users.router)` and `api_router.include_router(items.router)` after `import app.routers.items as items`
- **THEN** no `GET /users/{}` or `GET /items/{}` exists, and each such mount counts in `mounts_unresolved`

#### Scenario: Attribute mounts and literal loops
- **GIVEN** `app.router.include_router(users.router, prefix="/api")`, and separately `self.app.include_router(users.router, prefix="/api")` in a method, over `APIRouter(prefix="/users")` with `@router.get("/{x_id}")`; and `for r in (users.router, items.router): app.include_router(r, prefix="/api/v1")` on `app = FastAPI()`
- **THEN** the first two mint nothing and each counts in `mounts_unresolved`, and the loop mints `GET /api/v1/users/{}` and `GET /api/v1/items/{}`

