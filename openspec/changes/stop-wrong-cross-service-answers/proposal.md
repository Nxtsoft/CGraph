# Cross-service answers stop being wrong before they get wider

## Why

On 2026-09-29 the cross-service probe ran bin-v0.6.4 over eight real repositories (Turing: turing-webapp, turing-api, ml-backend, turing-agents; ModSquad: idp, idp-front-end, passless-app, passless-cli) against 81 hand-labelled cross-service dependencies. Coverage was low (12 of 41 and 4 of 40), and four answers were not just short but wrong. An agent that reads them is told something false:

1. **A cold workspace answers "nothing depends on this".** The first federated `impact` after the daemons start returned `{"ok":true,"total":0}`; the same query a moment later returned 9 nodes. A cold daemon answers at once from its empty graph (`daemon_server.cpp` builds on a worker thread), the per-repo answer is `found:false`, and federation reports `found:false, total:0`. The `building` list it attaches is easy to miss, and the MCP path returns the same shape.
2. **A call at the front of a URL is dropped as the host.** `${base(projectId)}/oracles` became `endpoint:POST /oracles`: `base()` returns `${BACKEND_URL}/api/v1/projects/${id}`, so the route was truncated and matched no provider (or could match the wrong one).
3. **Typed awaited requests vanish.** tree-sitter-typescript parses `await axios.post<T>(url)` as `(await axios.post)<T>(url)`, so the extractor saw an `await_expression` callee and dropped the call. idp-front-end has 31 such lines, including its login route.
4. **Fused graphs merge two repositories' files.** Node ids are project-relative, so turing-api and turing-agents both mint `src_db_client_ts`; `seam fuse` kept one node pointing at the turing-agents file with 304 turing-api import edges, and a table impact listed the wrong repository's file.

These come first because the plan that follows (surfacing cross-service consumers to agents unprompted) would otherwise put these wrong answers in front of every agent. Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`.

## What Changes

- **Client waits out a build.** `send_thin_client_request` re-asks a daemon whose answer carries `graph_state: "building"` for graph-reading ops (`query`, `path`, `explain`, `impact`, `context`, `report`, `recall`), backing off up to 250 ms per retry, until the build publishes or `ClientRequest::build_wait` (default 30 s) runs out. A workspace request shares one wait across every member ask and contract hop (a first version gave each ask its own 30 s; review measured 60.4 s for one member still building, past Codex's 60 s MCP tool timeout). On timeout the building answer is returned unchanged, so its marker still reaches the caller. `status`, `update`, `shutdown` and `remember` never wait. This is the one entry point for the CLI, the MCP server and every workspace member, so single-repo cold starts are fixed too.
- **A path builder is not a host.** A call at the front of a URL that takes a runtime value (`${base(projectId)}/...`) builds part of the path this file cannot see, so the request is left unresolved (counted in `calls_unresolved`) instead of minting a truncated route. A host getter there (no arguments, only string literals, or chained off `process.env`: `${getAgentsApiUrl()}`, `${config.get('apiUrl')}`, `${process.env.API_URL?.replace(/\/$/, '')}`) and a leading local or member (`${apiUrl}`, `${this.baseUrl}`) are still read as the host: a first, stricter version that also refused those was measured on the probe repositories and dropped 113 calls that had matched a real provider (62 in turing-webapp, 48 in idp-front-end, 3 in turing-agents) to remove about 80 unmatched ones.
- **Typed awaited calls are read.** `http_call_handler` reads the callee through the `await_expression` that tree-sitter-typescript wraps around it when the call has type arguments.
- **Fused ids are scoped by service.** `fuse_seam` prefixes every service-local node id with `<service>::`; `endpoint:`, `service:` and `schema:` ids stay shared so providers and consumers still meet. `seam discover` and `seam generate` stamp every edge into service code with a `service` property (and discover keeps one such edge per service, where it used to keep one per raw id), and fuse places each edge by that stamp; an unstamped edge into service code, from an older seam, fails the fuse rather than being guessed.
- `kIndexVersionKey` is bumped to `logic-8` because extraction output changes.

## Contract that tests verify

- A workspace asked while one member's build is held on a real FIFO (`client_runtime_test.cpp`, real daemons): with `build_wait` 0 the answer names `web` as `building` and has no `web` node; with 800 ms it returns within 1.5 s, still `building` (1,635 ms with a wait per ask); with the default wait and the build released at 400 ms it returns `web`'s consumer and no `building` list.
- `r = await axios.post<{ ok: boolean }>(\`${BACKEND_URL}/api/v1/assigned-generic\`)` and `const s = await axios.post<LoginResponse>(...)` each become an `http_call` with their path; `fetch(\`${base(projectId)}/oracles\`)` becomes an unresolved call with no path; `${config.baseUrl}/items`, `${getAgentsApiUrl()}/runs/wait`, `${process.env.NEXT_PUBLIC_API_URL?.replace(/\/$/, '')}/api/v1/users` and `${config.get('apiUrl')}/api/v1/orders` read their paths (`javascript_extractor_test.cpp`). On the old extractor the first two were missing and the third read `/oracles`.
- Two services that both own `src/db/client.ts` fuse into `api::src_db_client_ts` and `agents::src_db_client_ts`, each keeping its own import edges, while both still consume one shared `endpoint:GET /x` (`seam_test.cpp`).
- Two services whose callers share the raw id `src_api_ts` each keep a `CONSUMED_AT` edge through `seam discover`, stamped with their service, and fuse lands each on its own node; the same fragment with the stamps removed is refused (`seam_test.cpp`). On the previous version discover kept one of the two edges.

## Non-goals

- Resolving a leading call, member or local into its real value (following `base()` into its template, or `this.baseUrl` into its initializer). A member or local that holds part of the path, not just the host, still yields a truncated route: idp-front-end's `${this.baseUrl}/overview` with a base of `/api/v1/analytics` reads `/overview`. That needs initializer resolution, planned for the next change.
- Wrappers whose path is not the first argument (`mlBackendRequest(method, path)`).
- Scoping ids in live workspace federation, which already keys merged results by repository.

## Impact

- `src/client/client_runtime.cpp`, `src/client/include/cgraph/client_runtime.hpp`: `build_wait` and the settle loop.
- `src/engine/javascript_extractor.cpp`: typed-await callee; opaque leading interpolation is unresolved.
- `src/engine/seam.cpp`: service-scoped ids in `fuse_seam`.
- `src/engine/index_persistence.cpp`: `logic-8`.
- Fused `graph.json` node ids change (service-prefixed). Single-repo `graph.json` ids do not change. Measured on the probe repositories (pinned 2026-09-29): turing-webapp loses 10 consumer edges, all of them `${base(projectId)}/...` truncations that matched no provider (`POST /oracles`, `GET /runs/{}`, `GET /cluster`, ...), and gains 2 that match; idp-front-end gains 27 (22 matching a provider, including `POST /api/v1/auth/login`); turing-agents gains 3 (2 matching). No edge that matched a provider was lost.
