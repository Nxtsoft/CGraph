# Cross-service answers stop being wrong before they get wider

## Why

On 2026-09-29 the cross-service probe ran bin-v0.6.4 over eight real repositories (Turing: turing-webapp, turing-api, ml-backend, turing-agents; ModSquad: idp, idp-front-end, passless-app, passless-cli) against 81 hand-labelled cross-service dependencies. Coverage was low (12 of 41 and 4 of 40), and four answers were not just short but wrong. An agent that reads them is told something false:

1. **A cold workspace answers "nothing depends on this".** The first federated `impact` after the daemons start returned `{"ok":true,"total":0}`; the same query a moment later returned 9 nodes. A cold daemon answers at once from its empty graph (`daemon_server.cpp` builds on a worker thread), the per-repo answer is `found:false`, and federation reports `found:false, total:0`. The `building` list it attaches is easy to miss, and the MCP path returns the same shape.
2. **A call at the front of a URL is dropped as the host.** `${base(projectId)}/oracles` became `endpoint:POST /oracles`, and `${config.baseUrl}/items` became `/items`: a truncated route that can match the wrong provider.
3. **Typed awaited requests vanish.** tree-sitter-typescript parses `await axios.post<T>(url)` as `(await axios.post)<T>(url)`, so the extractor saw an `await_expression` callee and dropped the call. idp-front-end has 31 such lines, including its login route.
4. **Fused graphs merge two repositories' files.** Node ids are project-relative, so turing-api and turing-agents both mint `src_db_client_ts`; `seam fuse` kept one node pointing at the turing-agents file with 304 turing-api import edges, and a table impact listed the wrong repository's file.

These come first because the plan that follows (surfacing cross-service consumers to agents unprompted) would otherwise put these wrong answers in front of every agent. Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`.

## What Changes

- **Client waits out a build.** `send_thin_client_request` re-asks a daemon whose answer carries `graph_state: "building"` for graph-reading ops (`query`, `path`, `explain`, `impact`, `context`, `report`, `recall`), backing off up to 250 ms per retry, until the build publishes or `ClientRequest::build_wait` (default 30 s) runs out. On timeout the building answer is returned unchanged, so its marker still reaches the caller. `status`, `update`, `shutdown` and `remember` never wait. This is the one entry point for the CLI, the MCP server and every workspace member, so single-repo cold starts are fixed too.
- **Only an in-file host is dropped.** A leading interpolation is dropped as the host only when this file defines it from `process.env` (directly or through a module constant); a call, a member expression or a local at the front of a URL makes the call unresolved (counted in `calls_unresolved`) instead of minting a truncated route. The `dropped_host` state, which only this rule fed, is removed.
- **Typed awaited calls are read.** `http_call_handler` reads the callee through the `await_expression` that tree-sitter-typescript wraps around it when the call has type arguments.
- **Fused ids are scoped by service.** `fuse_seam` prefixes every service-local node id with `<service>::`; `endpoint:`, `service:` and `schema:` ids stay shared so providers and consumers still meet. Seam edges to a `code-ref` shadow land on the shadow's recorded service.
- `kIndexVersionKey` is bumped to `logic-8` because extraction output changes.

## Contract that tests verify

- A workspace asked while one member's build is held on a real FIFO returns that member's consumer of the endpoint and carries no `building` list (`client_runtime_test.cpp`, real daemons). With `build_wait` 0 the same request returns `building` and no consumer.
- `r = await axios.post<{ ok: boolean }>(\`${BACKEND_URL}/api/v1/assigned-generic\`)` and `const s = await axios.post<LoginResponse>(...)` each become an `http_call` with their path; `fetch(\`${base(projectId)}/oracles\`)` and `fetch(\`${config.baseUrl}/items\`)` become unresolved calls with no path (`javascript_extractor_test.cpp`). On the old extractor the first two were missing and the last two read `/oracles` and `/items`.
- Two services that both own `src/db/client.ts` fuse into `api::src_db_client_ts` and `agents::src_db_client_ts`, each keeping its own import edges, while both still consume one shared `endpoint:GET /x` (`seam_test.cpp`).

## Non-goals

- Resolving a leading call or member into a real prefix (following `base()` into its template). That is resolution work for the next change; this one only stops minting a wrong path.
- Wrappers whose path is not the first argument (`mlBackendRequest(method, path)`).
- Scoping ids in live workspace federation, which already keys merged results by repository.
- `discover_seam` deduplicating two services' shadows that share a raw id; only fuse is changed.

## Impact

- `src/client/client_runtime.cpp`, `src/client/include/cgraph/client_runtime.hpp`: `build_wait` and the settle loop.
- `src/engine/javascript_extractor.cpp`: typed-await callee; opaque leading interpolation is unresolved.
- `src/engine/seam.cpp`: service-scoped ids in `fuse_seam`.
- `src/engine/index_persistence.cpp`: `logic-8`.
- Fused `graph.json` node ids change (service-prefixed). Single-repo `graph.json` ids do not change. Some previously minted endpoints disappear from single-repo graphs where the URL began with a call or member; those calls now count as unresolved.
