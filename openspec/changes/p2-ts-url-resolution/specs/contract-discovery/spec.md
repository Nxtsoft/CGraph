## MODIFIED Requirements

### Requirement: HTTP client calls become CONSUMES edges to endpoints
For every call to `fetch`, to `<receiver>.<verb>` where the receiver's name contains api, client, axios, ky, got, http, fetch, request or agent and no argument is a function, or to a function that resolves (through the file's imports, then its own declarations) to a wrapper whose own such call appends one of its parameters to a fixed prefix, the pipeline SHALL reduce the URL argument to a path (literal text kept; a leading interpolation dropped as the host, except a call (`${base(id)}/x`) to a builder the file does not define, which leaves the request unresolved because the builder may hold part of the path, while a module function of the same file whose one return value reads as a path is inlined with its parameters as segment values; a whole-segment interpolation `{}`; module-level string constants inlined; a class field `this.X` set once to a string inlined, and one whose value is a path the file cannot read (`X || '/api/roles'`) making the request unresolvable; a block-scoped local standing for the whole URL read through its value when that value reads as a path and the local is set once (an initializer never reassigned, or assignments before the call in opposite branches of one if/else, giving one consumer per branch; a write in the other branch of an if/else from the read never reaches it unless a loop around the read carries it back), and a local reassigned (including by destructuring, `({ url } = cfg)` or `[url] = cfg`), built from itself, bound by a loop header or `catch`, assigned in another function or updated in place being unknowable; `new URL(x)`, `u.toString()` and `u.href` read as `x`; a local or same-file helper holding only a query string starting the query; `+` concatenation read as a template; query string and fragment cut), join an axios instance's `baseURL` (from `axios.create({ baseURL })` in a module constant or a `this.x` member) to the path the way axios does, read calls to a class method (`this.patch(...)`) or module function of the same file through the one shape its client calls agree on, compose wrapper prefix and argument, canonicalise it, take the method from the call's literal `method` option (one consumer per verb when it chooses between two verb literals) where the wrapper lets it (a wrapper's options object read member by member, a later member winning, so `{ method: 'GET', ...init }` takes the call's method and `{ ...options, method: 'PATCH' }` does not; options held in a local set once, a spread object literal, and the rest of a destructured options parameter read as their value; options whose method the file cannot read, being a value it cannot see, something other than a parameter spread in after any literal method, or a method neither a literal nor a parameter, leaving unresolved a direct client call, a `this.x(...)` call into a same-class wrapper, and a call forwarding a parameter into a same-file wrapper), else the wrapper's own literal method, else the client verb, else GET, and add a `CONSUMES` edge from the calling function (else the module-level variable the call initialises, else the file) to `endpoint:<METHOD> <canonical path>`, minting that node with `served: false`, no source and the canonical label when no route in the repository serves it. A wrapper whose path is a later parameter or whose method is a parameter SHALL be filled from the call's own arguments (a verb literal for the method, the parameter's literal default when the call leaves it out; for a wrapper fixing no method, the options right after the path, GET when they are left out or name no method); a call whose method is not spelled out, or whose options there the file cannot read, SHALL be counted unresolved. A call to a wrapper whose first parameter is the path, from any file, still takes an unreadable options argument's method from the wrapper's own literal method, else GET: the `http_wrapper` fact does not record which parameter holds the options, so the caller's options cannot be told from a body (a known gap). A path without a leading slash SHALL be joined only to a prefix ending in `/`. A URL held in a variable whose value cannot be read or led by a call it cannot read, an absolute `http(s)://` literal, or a path not starting with `/` that nothing joins SHALL add no edge and SHALL be counted in `route_resolution.calls_unresolved`; a call through a name that is no wrapper SHALL be neither an edge nor a count.

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

#### Scenario: A builder the file defines is read, one it imports is not
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
- **GIVEN** `apiRequest(endpoint)` fetching `\`${API_BASE}/${endpoint.replace(/^\//, '')}\`` and `apiFetch(path)` fetching `\`${API_BASE}${path}\`` with `API_BASE = '/api/backend'`, called as `apiRequest(\`v1/service-providers/${id}/refresh-metadata\`, { method: 'POST' })` and `apiFetch(\`v1/users/${id}\`)`
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
