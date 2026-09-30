# TypeScript client calls resolve the URL shapes real repositories use

## Why

The 2026-09-29 multi-service probe (81 hand-labelled cross-service edges) found TypeScript consumers CGraph could not read, even though every one is statically certain from its own file:

- wrappers whose path is not the first argument, or whose method is a parameter: turing-api `mlBackendRequest('POST', \`/project/${id}/setup\`)` forwarding into `mlRequest(baseUrl, method, path)` (T16-T19);
- class-method wrappers and axios instances with a `baseURL`: idp-front-end `this.patch<User>(\`v1/users/${id}/enable\`)` and `this.api.get(\`v1/public/...\`)` under `baseURL: '/api/backend'`, and `apiRequest(\`v1/service-providers/...\`)` whose own fetch strips the path's leading slash (M07, M08, M10);
- a URL held in a local or built with `new URL(...)` (M02, M03, M05, T26);
- a same-file URL builder (`const base = (projectId) => \`${API}/api/v1/projects/${projectId}/simulate\``, T06/T07) and a class field holding a prefix (`${this.API_BASE}/v1/passkeys/...`, M09).

Phase 0 deliberately left a leading builder call unresolved so it could not mint a truncated path; this change reads the builder instead when it is defined in the same file.

## What Changes

- **Locals.** A local standing for the whole URL (`const url = \`${BASE}/x\`; fetch(url)`) is read through its value when that value reads as a path. Only a local set once is read: an initializer with no later assignment, or, with no initializer, assignments before the read in opposite branches of one if/else (one consumer per branch). A local that is reassigned (`url = url + '/x'`, a second `url = ...`, a loop's `next = ...`), built from itself, bound by a `for` header or `catch`, assigned in another function (a test's `beforeAll`) or updated in place (`url +=`, `url.pathname = ...`) is unknowable and counted unresolved. A local that is a host (`'http://localhost:8080'`, a call) stays the host, and one inside the path stays a segment value, as before; one holding a query string (a same-file helper returning `''` or `?...`) starts the query.
- **`new URL(x)`, `url.toString()`, `url.href`** are the URL `x`. `new URL(path, base)` is unresolvable.
- **Same-file builders and query helpers.** A call to a module function of the file whose one return value reads as a path is inlined, its parameters filling segments. A helper returning only query strings starts the query. Anything else (an imported builder, a host getter) keeps the old reading.
- **Class fields.** `this.X` set once in the class (field initializer or one constructor assignment) to a string is inlined. A field whose value is some path we cannot read (`API_ENDPOINTS.roles || '/api/roles'`) makes the URL unresolvable instead of being dropped as a host.
- **Axios instances.** `x.get(path)` on a module constant or `this.x` created by `axios.create({ baseURL })` joins the base URL and the path the way axios does. A base URL this file cannot read makes the request unresolvable.
- **Wrappers in the same file.** A class method or module function whose client calls agree on one shape is a wrapper; `this.patch(\`v1/users/${id}/enable\`)` and a function forwarding its parameters into another wrapper (`mlBackendRequest` into `mlRequest`, which receives its base as a parameter) are read through it.
- **Positional wrappers across files.** A wrapper whose path is parameter `i` or whose method is parameter `j` is recorded as `@j[=DEFAULT] <prefix> #i`. Every non-primitive call with a path-like argument also records its arguments (`http_call_args`: `P<path>`, `V<VERB>`, `O<VERB>`), and `resolve_contracts` fills the wrapper's slots from them. A method parameter's literal default (`method = 'GET'`) applies when a call leaves it out. A call whose method is not spelled out is counted unresolved.
- **Relative paths.** A path without a leading slash (`v1/users`) is kept only when its client joins it to a base ending in `/` (an axios `baseURL`, `${API_BASE}/${path.replace(/^\//, '')}`). Otherwise it is counted unresolved, never glued onto a prefix without a slash.
- **Method through wrappers follows spread order.** A wrapper's options object is read member by member, a later member winning: `{ method: 'GET', ...init }` lets a call's own `method` override GET, `{ ...options, method: 'PATCH' }` is PATCH whatever the call passes. This holds when the wrapper is itself composed into another wrapper of the file (`authenticatedPatch` forwarding into `authenticatedFetch`).
- **Method choice.** `method: on ? 'POST' : 'DELETE'` gives one consumer per verb.
- **Destructured parameters** keep their position (`request({ a }, path)` takes the path at 1) and shadow outer locals of the same name.

## Contract that tests verify

The HTTP consumer reading lives in `src/engine/http_consumers.cpp` (moved out of `javascript_extractor.cpp`, which it had grown to 3108 lines); its assertions are in `tests/smoke/http_consumers_test.cpp`. The syntax helpers both files use are declared in `javascript_syntax.hpp`. Wrapper shapes are remembered per function for the file being extracted (`HttpConsumerFileScope`), so a class of many methods calling each other is read once per method instead of re-reading every callee at every call.

`tests/smoke/http_consumers_test.cpp`:
- `viaHelper` (`fetch(\`${base(projectId)}/oracles\`)` with `base` defined in the file) consumes `POST /api/v1/projects/{}/oracles`, and an imported builder stays unresolved (`POST ` with an empty path).
- The URL-shapes block yields the exact facts for a local template, `new URL(...).toString()`, a `let` assigned in both branches (two consumers), a URL whose `pathname` is rewritten (unresolved), a query helper, a slash-stripping module wrapper, an axios instance's `baseURL`, a `this.patch` class wrapper, a class-field prefix, and the `mlBackendRequest` -> `mlRequest` chain (`@0  #1`, plus the arguments fact `VPOST\tP/project/{}/setup\t`).
- The locals block pins the old readings that must not change (a literal-origin local and a call-built local at the host are the host, a `beforeAll`-assigned local mid-path is `{}`, a destructured parameter shadows an outer local), plus the default method (`@1=GET`), an in-file host getter, a ternary method, a path-fallback class field (unresolved, no `/hierarchy`), and a literal class field (`/api/analytics/export`).
- The review block: `this.request('/api/v1/widgets', { method: 'POST' })` into `request(path, init) { fetch(..., { method: 'GET', ...init }) }` consumes POST and the call without options GET; `authenticatedPatch` forwarding `{ ...options, method: 'PATCH' }` into `authenticatedFetch` is a `PATCH` wrapper; `concatLocal` (`url = url + '/tagged'`), `sequential` (two assignments), `forShadow` (`for (const url of urls)`) and `paged` (reassigned in a loop) record an empty path; `positioned({ tenant }, path)` records `/api/v1 #1`. Against the code before the review fixes it fails with `review fact: http_call|lib_review_ts_concatlocal|fetch| /tagged`, `... create|this.request|GET /api/v1/widgets`, `... authenticatedpatch|authenticatedFetch| ` and `... sequential|fetch| /api/v1/old`; with destructured parameters dropped from the position list it fails with `http_wrapper|lib_review_ts_positioned|fetch| /api/v1`.

`tests/smoke/contracts_test.cpp` `test_positional_and_relative_wrappers`: across two files, `setup` consumes `POST /project/{}/setup`, `cluster` consumes `GET /simulate/cluster`, `refresh` consumes `POST /api/backend/v1/service-providers/{}/refresh-metadata`, `poll` (method left out) consumes `GET /api/v1/notes/{}/presence` and `beat` consumes `POST`. An unknown method and a relative path glued onto `/api/backend` with no slash mint nothing; the tally is 7 calls, 2 unresolved, 5 consumes.

The first-round tests fail against the code before this change (source files restored from HEAD): the extractor test stops at `http_call|lib_extra_ts_viahelper|fetch|POST ` and the contracts test at `positional and slash-joined wrapper consumers`, with `poll` and `beat` both consuming `GET`.

## Measured on the probe repositories

Pinned probe copies, `score.py`, baseline bin-v0.6.7 build vs this change:

- Turing links 12 -> 15 of 41 (HTTP 12 -> 15 of 25): T06, T07, T26 now link. T16-T19 consumers now produce `POST /project/{}/setup`, `GET /project/{}/formulations/score`, `POST /simulate/oracles/from-project` and `GET /simulate/cluster`; they link once the Python routes land (#143). T22's consumer (`POST /runs/stream`) now exists; its provider is a LangGraph platform route no graph declares. T08 (a path stored in an object literal and fetched generically) stays unresolved: that shape does not generalise.
- ModSquad links 4 -> 7 of 40 (HTTP 4 -> 7 of 29): M02, M03, M05 now link. M07-M10 now produce `PATCH /api/backend/v1/users/{}/enable`, `GET /api/backend/v1/public/tenants/{}/theme/css`, `POST /api/backend/v1/passkeys/register/start` and `POST /api/backend/v1/service-providers/{}/refresh-metadata`; the `/api/backend` -> `/api` proxy mapping (#144) joins them to idp.
- CONSUMES edges: turing-webapp 520 -> 633, turing-api 30 -> 48, turing-agents 47 -> 51, idp-front-end 528 -> 710 (the review fix adds `useBackupCodesSettings` -> `PATCH /api/backend/v1/tenants/{}/settings/backup-codes`). Previously matched edges lost: 0 in all four. Unresolved calls: turing-webapp 117 -> 23, idp-front-end 217 -> 196, turing-api 13 -> 10, turing-agents 8 -> 9.
- Cross-service seam links: Turing 508 -> 620, ModSquad 194 -> 237, none removed. 20 sampled new links checked against source: 20 correct.
- Build time (successful runs; mars segfaults in bursts on both binaries): turing-webapp 18.3 s, 18.2 s, 17.3 s at bin-v0.6.7 vs 19.0 s, 18.0 s; idp-front-end 6.8 s, 6.8 s vs 6.9 s, 7.1 s. The reviewer's 30-method class fixture (691 lines): 0.13 s at bin-v0.6.7, 90.8 s before the cache, 0.23 s after.
- Three idp-front-end calls in `services/theme.service.ts` (`this.api.get('/api/backend/v1/public/tenants/${tenantId}/theme/config')` and two more) go unresolved: the instance is `axios.create({ baseURL: this.config.baseURL })` with `this.config = { baseURL: '', ..., ...config }`, so a caller's config may replace the base. Once the `/api/backend` proxy mapping (#144) lands, idp serves them, so they are 3 matches this change does not make; no certain reading exists.
- Truncated consumers removed: idp-front-end `POST /export`, `GET /hierarchy`-style paths from class fields and `/v1/passkeys/...` without `this.API_BASE` now read the full path or stay unresolved.

## Non-goals

- Proxy route mapping (`/api/backend/*` forwarding to the backend's `/api/*`): #144.
- A path stored in data and fetched generically (T08).
- Cross-file builders and base URLs beyond the existing project-wide URL constant inliner.
- A subclass overriding a field its base class builds URLs from (`class Admin extends Base { API_BASE = '/api/admin' }`): the base class's own value is read.

## Impact

`src/engine/http_consumers.cpp` (new; URL reading, wrapper shapes, `http_call_args`), `src/engine/javascript_extractor.cpp` (the section moved out, shared helpers in `js_syntax`), `src/engine/contracts.cpp` (additive: positional wrapper facts and their calls, the relative-path join rule), and their tests. Graphs built before this change are rebuilt by the index version bump the orchestrator applies per PR.
