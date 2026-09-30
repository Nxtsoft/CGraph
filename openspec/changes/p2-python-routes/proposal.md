# Python FastAPI services are HTTP providers

## Why

ml-backend, Turing's Python ML service, served 0 endpoints in its graph on bin-v0.6.7 (`route_resolution.routes` 0 in the probe's `graphs.base/ml-backend/stats.json`), so turing-api's calls into it had no provider: ground-truth edges T16-T20 (`probe-base067/ground_truth_turing.json`) all carried the miss cause `no_python_routes` (`miss_causes.json`). Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 2.

## What Changes

- **The Python extractor emits the same three contract facts the JavaScript extractor does.** A module-level `x = APIRouter(prefix="/p")` or `x = FastAPI(...)` is a `variable` node (contained by its file) carrying `route_prefix`; a def decorated `@x.<verb>(path, ...)` (get/post/put/patch/delete/head/options) or `@x.api_route(path, methods=[...])` (GET when `methods` is absent) is a `route` fact per method handled by the def; `x.include_router(y, prefix="/q")` is a `mounts` fact. `resolve_contracts` composes them exactly as it composes Express/Elysia chains.
- **Aliased Python imports bind their alias.** `from m import router as setup_router` puts `alias` on the file's own `imports` edge (not the shared stub), so `build_relation_scopes` resolves `setup_router` in that file.
- **Unplaceable mounts are counted, not guessed.** A mount inside a function (an app factory) or with a non-literal `prefix` is recorded with no mounting chain; `resolve_contracts` counts it in `mounts_unresolved` and, when it is the child's only mount, gives the child no path, so its routes count as `routes_unresolved` instead of minting at a wrong top-level path. A decorated def inside a function, a non-literal path or `methods`, and a router whose `prefix` is not a literal likewise mint nothing and are counted.

## Contract that tests verify

- `python_extractor_test.cpp` `fastapi_routes`: six files in ml-backend's layout (`api/app.py`, `api/routes/project/router.py`, two route modules, `api/legacy/__init__.py`, `api/legacy/routes.py`) mint exactly `GET /healthcheck`, `POST /project/{}/setup`, `GET /project/{}/formulations/score` (multi-line decorator), `GET /simulate/cluster` (empty path under a prefix), `POST` and `PUT /simulate/cluster/wake` (`api_route`), and `GET /v1/model/{}/variables` (include_router prefix through a package `__init__`), each `handled_by` its def and `contains`-ed by its file; the label keeps `{project_id}`; the router import binds the variable its file contains; `routes` 11, `routes_unresolved` 4 (factory route, non-literal router prefix, router whose only mount has a non-literal prefix, f-string path), `mounts` 7, `mounts_unresolved` 2.
- Fails on origin/main (`missing endpoint:GET /healthcheck`), and with only the contracts.cpp change reverted (`endpoint count 8`: the router behind the unplaceable mount mints `GET /dynamic`).

## Measured on the probe repositories

- ml-backend: `route_resolution` routes 40, routes_unresolved 0, mounts 25, mounts_unresolved 0, endpoints 40 (was all 0); every one of the 40 `@<router>.<verb>(` decorators in the repo mints one endpoint, 10 spot-checked against their prefixes and mounts.
- Turing score: HTTP links 12 -> 13 of 25 (T20 now links); provider_node 16 -> 21 (T16-T20). T16-T19 have their provider endpoint and wait on the consumer side (wrapper `mlBackendRequest` resolution).
- New cross-service seam rows: 1 (`GET /healthcheck` turing-api `health.ts` -> ml-backend `api/app.py`), correct; lost rows 0 in Turing and ModSquad. No other probe repository's graph changed (node and edge sets identical).

## Non-goals

- Flask and Blueprints: `register_blueprint(bp, url_prefix=)` replaces the blueprint's own prefix, which the mount walk does not express.
- App factories (`def create_app(): app = FastAPI(); app.include_router(...)`), `app.add_api_route(...)`, `include_router(module.router)` (a dotted router): counted unresolved.
- Python HTTP clients (`requests`/`httpx`) as consumers.

## Impact

- `src/engine/python_extractor.cpp`: router variables, route decorators, include_router mounts, import alias on the edge.
- `src/engine/contracts.cpp`: a mount with no mounting chain is counted and leaves an otherwise unmounted child without a path.
- `tests/smoke/python_extractor_test.cpp`.
- Graph output for Python repositories gains `variable`, `endpoint` nodes and `contains`, `handled_by`, `mounts` edges; a `from m import router` import now targets the router variable instead of its file. The index version bump is the orchestrator's.
