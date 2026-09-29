# Cross-service answers stop being wrong before they get wider

## Why

On 2026-09-29 the cross-service probe ran bin-v0.6.4 over eight real repositories (Turing: turing-webapp, turing-api, ml-backend, turing-agents; ModSquad: idp, idp-front-end, passless-app, passless-cli) against 81 hand-labelled cross-service dependencies. Coverage was low (12 of 41 and 4 of 40), and four answers were not just short but wrong. An agent that reads them is told something false:

1. **A cold workspace answers "nothing depends on this".** The first federated `impact` after the daemons start returned `{"ok":true,"total":0}`; the same query a moment later returned 9 nodes. A cold daemon answers at once from its empty graph (`daemon_server.cpp` builds on a worker thread), the per-repo answer is `found:false`, and federation reports `found:false, total:0`. The `building` list it attaches is easy to miss, and the MCP path returns the same shape.
2. **A call at the front of a URL is dropped as the host.** `${base(projectId)}/oracles` became `endpoint:POST /oracles`: `base()` returns `${BACKEND_URL}/api/v1/projects/${id}`, so the route was truncated and matched no provider (or could match the wrong one).
3. **Typed awaited requests vanish.** tree-sitter-typescript parses `await axios.post<T>(url)` as `(await axios.post)<T>(url)`, so the extractor saw an `await_expression` callee and dropped the call. idp-front-end has 31 such lines, including its login route.
4. **Fused graphs merge two repositories' files.** Node ids are project-relative, so turing-api and turing-agents both mint `src_db_client_ts`; `seam fuse` kept one node pointing at the turing-agents file with 304 turing-api import edges, and a table impact listed the wrong repository's file.

These come first because the plan that follows (surfacing cross-service consumers to agents unprompted) would otherwise put these wrong answers in front of every agent. Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`.

## What Changes

- **Client waits out a build.** `send_thin_client_request` re-asks a daemon whose answer carries `graph_state: "building"` for graph-reading ops (`query`, `path`, `explain`, `impact`, `context`, `report`, `recall`), backing off up to 250 ms per retry, until the build publishes or `ClientRequest::build_wait` (default 30 s) runs out. On timeout the building answer is returned unchanged, so its marker still reaches the caller. `status`, `update`, `shutdown` and `remember` never wait. This is the one entry point for the CLI, the MCP server and every workspace member, so single-repo cold starts are fixed too.
- **A leading call is not a host.** A call at the front of a URL (`${base(id)}/...`) is a URL builder whose result may end in a path this file cannot see, so the request is left unresolved (counted in `calls_unresolved`) instead of minting a truncated route. A leading local or member (`${apiUrl}`, `${this.baseUrl}`) is still read as the host: a first, stricter version that also refused those was measured on the probe repositories and dropped 113 calls that had matched a real provider (62 in turing-webapp, 48 in idp-front-end, 3 in turing-agents) to remove about 80 unmatched ones.
- **Typed awaited calls are read.** `http_call_handler` reads the callee through the `await_expression` that tree-sitter-typescript wraps around it when the call has type arguments.
- **Fused ids are scoped by service.** `fuse_seam` prefixes every service-local node id with `<service>::`; `endpoint:`, `service:` and `schema:` ids stay shared so providers and consumers still meet. Seam edges to a `code-ref` shadow land on the shadow's recorded service.
- `kIndexVersionKey` is bumped to `logic-8` because extraction output changes.

## Contract that tests verify

- A workspace asked while one member's build is held on a real FIFO returns that member's consumer of the endpoint and carries no `building` list (`client_runtime_test.cpp`, real daemons). With `build_wait` 0 the same request returns `building` and no consumer.
- `r = await axios.post<{ ok: boolean }>(\`${BACKEND_URL}/api/v1/assigned-generic\`)` and `const s = await axios.post<LoginResponse>(...)` each become an `http_call` with their path; `fetch(\`${base(projectId)}/oracles\`)` becomes an unresolved call with no path, while `fetch(\`${config.baseUrl}/items\`)` still reads `/items` (`javascript_extractor_test.cpp`). On the old extractor the first two were missing and the third read `/oracles`.
- Two services that both own `src/db/client.ts` fuse into `api::src_db_client_ts` and `agents::src_db_client_ts`, each keeping its own import edges, while both still consume one shared `endpoint:GET /x` (`seam_test.cpp`).

## Non-goals

- Resolving a leading call, member or local into its real value (following `base()` into its template, or `this.baseUrl` into its initializer). A member or local that holds part of the path, not just the host, still yields a truncated route: idp-front-end's `${this.baseUrl}/overview` with a base of `/api/v1/analytics` reads `/overview`. That needs initializer resolution, planned for the next change.
- Wrappers whose path is not the first argument (`mlBackendRequest(method, path)`).
- Scoping ids in live workspace federation, which already keys merged results by repository.
- `discover_seam` deduplicating two services' shadows that share a raw id; only fuse is changed.

## Impact

- `src/client/client_runtime.cpp`, `src/client/include/cgraph/client_runtime.hpp`: `build_wait` and the settle loop.
- `src/engine/javascript_extractor.cpp`: typed-await callee; opaque leading interpolation is unresolved.
- `src/engine/seam.cpp`: service-scoped ids in `fuse_seam`.
- `src/engine/index_persistence.cpp`: `logic-8`.
- Fused `graph.json` node ids change (service-prefixed). Single-repo `graph.json` ids do not change. Measured on the probe repositories (pinned 2026-09-29): turing-webapp loses 12 consumer edges, none of which matched a provider (`POST /oracles`, `POST /runs`, `GET /cluster`, ...), and gains 2 that match; idp-front-end gains 27 (22 matching a provider, including `POST /api/v1/auth/login`); turing-agents gains 3 (2 matching). No edge that matched a provider was lost.
