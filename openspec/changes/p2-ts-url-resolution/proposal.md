# TypeScript client calls resolve the URL shapes real repositories use

## Why

The 2026-09-29 multi-service probe (81 hand-labelled cross-service edges) found TypeScript consumers CGraph could not read, even though every one is statically certain from its own file:

- wrappers whose path is not the first argument, or whose method is a parameter: turing-api `mlBackendRequest('POST', \`/project/${id}/setup\`)` forwarding into `mlRequest(baseUrl, method, path)` (T16-T19);
- class-method wrappers and axios instances with a `baseURL`: idp-front-end `this.patch<User>(\`v1/users/${id}/enable\`)` and `this.api.get(\`v1/public/...\`)` under `baseURL: '/api/backend'`, and `apiRequest(\`v1/service-providers/...\`)` whose own fetch strips the path's leading slash (M07, M08, M10);
- a URL held in a local or built with `new URL(...)` (M02, M03, M05, T26);
- a same-file URL builder (`const base = (projectId) => \`${API}/api/v1/projects/${projectId}/simulate\``, T06/T07) and a class field holding a prefix (`${this.API_BASE}/v1/passkeys/...`, M09).

Phase 0 deliberately left a leading builder call unresolved so it could not mint a truncated path; this change reads the builder instead when it is defined in the same file.

## What Changes

- **Locals.** A local standing for the whole URL (`const url = \`${BASE}/x\`; fetch(url)`) is read through its value when that value reads as a path; a `let` assigned per branch before the call gives one consumer per value. A local assigned in another function (a test's `beforeAll`) or updated in place (`url +=`, `url.pathname = ...`) is unknowable. A local that is a host (`'http://localhost:8080'`, a call) stays the host, and one inside the path stays a segment value, as before; one holding a query string (a same-file helper returning `''` or `?...`) starts the query.
- **`new URL(x)`, `url.toString()`, `url.href`** are the URL `x`. `new URL(path, base)` is unresolvable.
- **Same-file builders and query helpers.** A call to a module function of the file whose one return value reads as a path is inlined, its parameters filling segments. A helper returning only query strings starts the query. Anything else (an imported builder, a host getter) keeps the old reading.
- **Class fields.** `this.X` set once in the class (field initializer or one constructor assignment) to a string is inlined. A field whose value is some path we cannot read (`API_ENDPOINTS.roles || '/api/roles'`) makes the URL unresolvable instead of being dropped as a host.
- **Axios instances.** `x.get(path)` on a module constant or `this.x` created by `axios.create({ baseURL })` joins the base URL and the path the way axios does. A base URL this file cannot read makes the request unresolvable.
- **Wrappers in the same file.** A class method or module function whose client calls agree on one shape is a wrapper; `this.patch(\`v1/users/${id}/enable\`)` and a function forwarding its parameters into another wrapper (`mlBackendRequest` into `mlRequest`, which receives its base as a parameter) are read through it.
- **Positional wrappers across files.** A wrapper whose path is parameter `i` or whose method is parameter `j` is recorded as `@j[=DEFAULT] <prefix> #i`. Every non-primitive call with a path-like argument also records its arguments (`http_call_args`: `P<path>`, `V<VERB>`, `O<VERB>`), and `resolve_contracts` fills the wrapper's slots from them. A method parameter's literal default (`method = 'GET'`) applies when a call leaves it out. A call whose method is not spelled out is counted unresolved.
- **Relative paths.** A path without a leading slash (`v1/users`) is kept only when its client joins it to a base ending in `/` (an axios `baseURL`, `${API_BASE}/${path.replace(/^\//, '')}`). Otherwise it is counted unresolved, never glued onto a prefix without a slash.
- **Method choice.** `method: on ? 'POST' : 'DELETE'` gives one consumer per verb.
- **Destructured parameters** keep their position (`request({ a }, path)` takes the path at 1) and shadow outer locals of the same name.

## Contract that tests verify

`tests/smoke/javascript_extractor_test.cpp`:
- `viaHelper` (`fetch(\`${base(projectId)}/oracles\`)` with `base` defined in the file) consumes `POST /api/v1/projects/{}/oracles`, and an imported builder stays unresolved (`POST ` with an empty path).
- The URL-shapes block yields the exact facts for a local template, `new URL(...).toString()`, a `let` assigned in both branches (two consumers), a URL whose `pathname` is rewritten (unresolved), a query helper, a slash-stripping module wrapper, an axios instance's `baseURL`, a `this.patch` class wrapper, a class-field prefix, and the `mlBackendRequest` -> `mlRequest` chain (`@0  #1`, plus the arguments fact `VPOST\tP/project/{}/setup\t`).
- The locals block pins the old readings that must not change (a literal-origin local and a call-built local at the host are the host, a `beforeAll`-assigned local mid-path is `{}`, a destructured parameter shadows an outer local), plus the default method (`@1=GET`), an in-file host getter, a ternary method, a path-fallback class field (unresolved, no `/hierarchy`), and a literal class field (`/api/analytics/export`).

`tests/smoke/contracts_test.cpp` `test_positional_and_relative_wrappers`: across two files, `setup` consumes `POST /project/{}/setup`, `cluster` consumes `GET /simulate/cluster`, `refresh` consumes `POST /api/backend/v1/service-providers/{}/refresh-metadata`, `poll` (method left out) consumes `GET /api/v1/notes/{}/presence` and `beat` consumes `POST`. An unknown method and a relative path glued onto `/api/backend` with no slash mint nothing; the tally is 7 calls, 2 unresolved, 5 consumes.

Both tests fail against the code before this change (source files restored from HEAD): the extractor test stops at `http_call|lib_extra_ts_viahelper|fetch|POST ` and the contracts test at `positional and slash-joined wrapper consumers`, with `poll` and `beat` both consuming `GET`.

## Measured on the probe repositories

Pinned probe copies, `score.py`, baseline bin-v0.6.7 build vs this change:

- Turing links 12 -> 15 of 41 (HTTP 12 -> 15 of 25): T06, T07, T26 now link. T16-T19 consumers now produce `POST /project/{}/setup`, `GET /project/{}/formulations/score`, `POST /simulate/oracles/from-project` and `GET /simulate/cluster`; they link once the Python routes land (#143). T22's consumer (`POST /runs/stream`) now exists; its provider is a LangGraph platform route no graph declares. T08 (a path stored in an object literal and fetched generically) stays unresolved: that shape does not generalise.
- ModSquad links 4 -> 7 of 40 (HTTP 4 -> 7 of 29): M02, M03, M05 now link. M07-M10 now produce `PATCH /api/backend/v1/users/{}/enable`, `GET /api/backend/v1/public/tenants/{}/theme/css`, `POST /api/backend/v1/passkeys/register/start` and `POST /api/backend/v1/service-providers/{}/refresh-metadata`; the `/api/backend` -> `/api` proxy mapping (#144) joins them to idp.
- CONSUMES edges: turing-webapp 520 -> 633, turing-api 30 -> 48, turing-agents 47 -> 51, idp-front-end 528 -> 709. Previously matched edges lost: 0 in all four. Unresolved calls: turing-webapp 117 -> 23, idp-front-end 217 -> 196, turing-api 13 -> 10, turing-agents 8 -> 9.
- Cross-service seam links: Turing 508 -> 620, ModSquad 194 -> 237, none removed. 20 sampled new links checked against source: 20 correct.
- Truncated consumers removed: idp-front-end `POST /export`, `GET /hierarchy`-style paths from class fields and `/v1/passkeys/...` without `this.API_BASE` now read the full path or stay unresolved.

## Non-goals

- Proxy route mapping (`/api/backend/*` forwarding to the backend's `/api/*`): #144.
- A path stored in data and fetched generically (T08).
- Cross-file builders and base URLs beyond the existing project-wide URL constant inliner.

## Impact

`src/engine/javascript_extractor.cpp` (URL reading, wrapper shapes, `http_call_args`), `src/engine/contracts.cpp` (additive: positional wrapper facts and their calls, the relative-path join rule), and their tests. Graphs built before this change are rebuilt by the index version bump the orchestrator applies per PR.
