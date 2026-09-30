# Routes a framework declares, and proxies between repositories

## Why

On the 2026-09-29 probe (`~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/`) two causes kept real cross-service calls from meeting their provider:

- `provider_not_code_route`: turing-agents is a LangGraph Agent Server. Its routes (`POST /runs/wait`, `POST /threads/{thread_id}/runs/wait`, ...) are registered by the framework, and `langgraph.json` only names the graphs. turing-webapp (T14) and turing-api (T21, T22) call them, and nothing in turing-agents' source serves them.
- `bff_proxy_prefix`: idp-front-end calls `/api/backend/v1/...`; its Next.js catch-all `app/api/backend/[...path]/route.ts` forwards to `${BACKEND_URL}/api/${path}` (route.ts:105). 56 of idp-front-end's consumed-with-no-provider endpoints are `/api/backend/*`, and none met idp's `/api/*`.

## What Changes

- **`langgraph.json` serves the Agent Server routes.** `detect_language` recognizes `langgraph.json`; the new `langgraph_config.cpp` extractor emits the file node, a `langgraph_server` node spanning the `graphs` object, one `langgraph_graph` node per graph (with `entrypoint`), and one `file_route` per server route, so `resolve_contracts` mints `endpoint:<METHOD> <path>` nodes `handled_by` the server. The table is the 53 routes `@langchain/langgraph-api` 1.5.1 registers (`dist/api/{meta,assistants,runs,threads,store,protocol}.mjs`, mounted in `dist/server.mjs`; Agent Server API reference https://docs.langchain.com/langsmith/server-api-ref). A group the config switches off (`http.disable_<group>`) is not served; a config without a non-empty `graphs` object declares no server.
- **Proxy prefixes where repositories meet.** A prefix `{repo, from, to}` (new `endpoint_prefixes.cpp`) says an endpoint `repo` consumes but does not serve, under `from`, reaches the other repositories at `to` + the rest. `seam discover` and `seam fuse` take `--prefix REPO:/from=/to`; `cgraph.workspace.json` takes `prefixes`. Discover joins such an endpoint at the proxied id, keeps the consumer's own path as `via` on `CONSUMED_AT`, never joins a path only the consumer itself serves, and logs the count per prefix. Fuse redirects the consumer's `CONSUMES` edge to the joined endpoint and refuses (fails loud) when the joined endpoint is in neither the seam nor a service graph. Workspace `impact` and `path` cross at the proxied contract both ways; a path keeps both spellings. Single-repo graph ids are unchanged.
- **Declared, not detected.** The forwarding target is built at run time from the handler's `params.path` through two functions (`handleProxyRequest` -> `forwardRequest`, route.ts:105 `${BACKEND_URL}/api/${path}${queryString}`, method from `request.method`), and the graph carries no `CONSUMES` edge from that file. Reading it would need the JavaScript extractor to follow parameters across calls; guessing `to` from the catch-all's own path would be wrong for any proxy that does not strip exactly `/backend`. So the prefix is declared.

## Contract that tests verify

- `langgraph_config_test.cpp`: server node spans the `graphs` object; one graph node per entry on its own line with `entrypoint`; all 53 routes as `file_route` from the server; `disable_store` drops the store routes; no `graphs` yields the file node only; through the real pipeline, `endpoint:POST /runs/wait` is served (no `served:false`), `handled_by` the server, and an in-repo `fetch` consumes it.
- `detect_test.cpp`: `langgraph.json` is `langgraph-config`; `package.json` stays undetected.
- `endpoint_prefixes_test.cpp`: mapping on a segment boundary only, in the named repo only; `to` of `/`; the inverse spelling; parsing, normalization, round-trip, CLI flag.
- `seam_test.cpp` `test_proxy_prefix`: without a prefix nothing joins; with it the proxied call joins the provider with `via`, a route the consumer serves (`/api/backend/healthz`) stays local, a proxied path only the consumer serves (`/api/saml/metadata`) does not join, the log line counts 1; fuse redirects `CONSUMES` and refuses a seam without the joined endpoint.
- `workspace_test.cpp`: manifest `prefixes` load normalized, round-trip, and a prefix naming no member or a relative path is an error; `impact` crosses from provider to the consumer's proxied spelling and back, not through a route the front end serves itself, not asked back of the repo it was proxied from; `path` joins keeping both spellings.
- Each of these fails on the code before the change (seam/workspace/langgraph_config/detect tests exit 1 with the HEAD sources).

## Measured on the probe repositories

- Turing: 14/41 linked (HTTP 14/25), was 12/41 (12/25). New: T14 (turing-webapp `app/api/observations/route.ts` -> `POST /runs/wait`) and T21 (turing-api `compiq/service.ts` -> `POST /runs`). Seam cross-service links 508 -> 534, 26 added, all 26 checked against source and correct.
- ModSquad: 5/40 linked (HTTP 5/29), was 4/40. New: M11. 56 `/api/backend/*` consumed endpoints mapped; 54 meet an idp endpoint, the other 2 (`/api/users/profile`, `/api/saml/metadata/{}`) have no idp route. Seam cross-service links 194 -> 265; 20 sampled, 20 correct.
- Lost: 0 cross-service links and 0 `CONSUMES` edges lost in any probe graph.
- Live workspace (probe copies, manifests with absolute roots): `impact` from turing-agents' `langgraph_server` reaches `app_api_observations_route_ts_post` (turing-webapp, via `POST /runs/wait`) and `src_modules_compiq_service_ts_startcompiqrun` (turing-api, via `POST /runs`); `path` from idp-front-end `togglelistingenabled` to idp's `toggleenabled` goes through both spellings of `PATCH .../toggle-enabled`.

## Non-goals

- T22: the call at turing-api `src/modules/luna/index.ts:318` sits inside an Elysia `.post('/stream', ...)` handler and yields no `CONSUMES` edge (the same `agentUrl` pattern in `framer-call.ts:41` does); that is the JavaScript extractor's attribution, not this change's.
- T15: `client.runs.stream(...)` through the LangGraph JS SDK; mapping SDK methods to routes is not done here.
- M23 Spring Boot Actuator `/actuator/health`: not implemented.
- Detecting the proxy target from code (see above).

## Impact

- New: `src/engine/langgraph_config.cpp`, `endpoint_prefixes.cpp` (+ headers, tests).
- `src/engine/detect.cpp`, `non_grammar_extractors.cpp`: `LangGraphConfig` language.
- `src/engine/seam.cpp`, `seam.hpp`, `src/cli/main.cpp`: `--prefix` for discover and fuse.
- `src/engine/workspace.cpp`, `workspace.hpp`: manifest `prefixes`; impact/path crossing.
- Graph output changes for repositories with a `langgraph.json` (new nodes and endpoints), so the index version must be bumped with this change (left to the orchestrator).
