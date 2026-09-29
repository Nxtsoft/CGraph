## MODIFIED Requirements

### Requirement: HTTP client calls become CONSUMES edges to endpoints
For every call to `fetch`, to `<receiver>.<verb>` where the receiver's name contains api, client, axios, ky, got, http, fetch, request or agent and no argument is a function, or to a function that resolves (through the file's imports, then its own declarations) to a wrapper whose own such call appends its first parameter to a fixed prefix, the pipeline SHALL reduce the URL argument to a path (literal text kept; a leading interpolation dropped as the host, except a call (`${base(id)}/x`), which leaves the request unresolved because the builder may hold part of the path; a whole-segment interpolation `{}`; module-level string constants inlined; `+` concatenation read as a template; query string and fragment cut), compose wrapper prefix and argument, canonicalise it, take the method from the call's literal `method` option, else the wrapper's own literal method, else the client verb, else GET, and add a `CONSUMES` edge from the calling function (else the module-level variable the call initialises, else the file) to `endpoint:<METHOD> <canonical path>`, minting that node with `served: false`, no source and the canonical label when no route in the repository serves it; a URL held in a variable or led by a call, an absolute `http(s)://` literal, or a path not starting with `/` SHALL add no edge and SHALL be counted in `route_resolution.calls_unresolved`; a call through a name that is no wrapper SHALL be neither an edge nor a count.

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
- **GIVEN** `const base = (id) => \`${BACKEND_URL}/api/v1/projects/${id}\`` and `fetch(\`${base(projectId)}/oracles\`, { method: 'POST' })` in `viaHelper`, and `fetch(\`${config.baseUrl}/items\`)` in `viaMember`
- **THEN** no `endpoint:POST /oracles` exists and that call is counted in `calls_unresolved`, while `viaMember` consumes `endpoint:GET /items`

#### Scenario: A typed awaited request is a request
- **GIVEN** `r = await axios.post<{ ok: boolean }>(\`${BACKEND_URL}/api/v1/assigned-generic\`, {})` and `const s = await axios.post<LoginResponse>(\`${BACKEND_URL}/api/v1/login\`, {})` in `typedAwait`, where `BACKEND_URL` is built from `process.env`
- **THEN** `typedAwait` consumes `endpoint:POST /api/v1/assigned-generic` and `endpoint:POST /api/v1/login`

