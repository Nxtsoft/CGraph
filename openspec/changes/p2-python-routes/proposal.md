# Python FastAPI services are HTTP providers

## Why

ml-backend, Turing's Python ML service, served 0 endpoints in its graph on bin-v0.6.7 (`route_resolution.routes` 0 in the probe's `graphs.base/ml-backend/stats.json`), so turing-api's calls into it had no provider: ground-truth edges T16-T20 (`probe-base067/ground_truth_turing.json`) all carried the miss cause `no_python_routes` (`miss_causes.json`). Plan: `~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 2.

## What Changes

- **The Python extractor emits the same three contract facts the JavaScript extractor does.** A module-level `x = APIRouter(prefix="/p")` or `x = FastAPI(...)` is a `variable` node (contained by its file) carrying `route_prefix`; a def decorated `@x.<verb>(path, ...)` (get/post/put/patch/delete/head/options) or `@x.api_route(path, methods=[...])` (GET when `methods` is absent) is a `route` fact per method handled by the def; `x.include_router(y, prefix="/q")` and `x.mount("/q", y)` (a sub-application; a `mount` whose path does not start with `/`, like `session.mount("https://", adapter)`, or whose app is not a name is not one) are `mounts` facts. `resolve_contracts` composes them exactly as it composes Express/Elysia chains.
- **An aliased Python import binds only its alias.** `from m import router as setup_router` puts `alias` on the file's own `imports` edge (not the shared stub), and `build_relation_scopes` binds that alias and, for an edge from a `.py` file, no longer also the imported name, so the file's own `router` stays its own. JavaScript is unchanged: its alias lives on the import stub every importer shares and is copied onto each importer's edge, so it may be another file's alias, and there both the name and the alias still bind (`graph_builder_test.cpp` `check_shared_stub_alias_binds_both_names`).
- **Python imports reach what they name.** In `resolve_imports`, a package `__init__.py` re-exports what it imports (under its alias), so `from api.routes import items_router` follows to items.py's `router`; a name the package neither declares nor re-exports is its submodule (`from app.routers import users` -> app/routers/users.py); and the importing file's edge carries the name it uses when that differs from the target's label. A Python symbol stub id now ends in the name's length, because `make_id` folds `/`, `:` and `_` alike and `from api.routes import items_router` and `from api.routes.items import router` otherwise shared one stub. `resolve_contracts` resolves a Python `module.router` mount through the imported module file.
- **Unplaceable mounts are counted, not guessed.** A mount inside a function (an app factory) or with a non-literal `prefix` is recorded with no mounting chain; `resolve_contracts` counts it in `mounts_unresolved` and, when it is the child's only mount, gives the child no path, so its routes count as `routes_unresolved` instead of minting at a wrong top-level path. A decorated def inside a function, a non-literal path or `methods`, and a router whose `prefix` is not a literal likewise mint nothing and are counted.

## Contract that tests verify

- `python_extractor_test.cpp` `fastapi_routes`: six files in ml-backend's layout (`api/app.py`, `api/routes/project/router.py`, two route modules, `api/legacy/__init__.py`, `api/legacy/routes.py`) mint exactly `GET /healthcheck`, `POST /project/{}/setup`, `GET /project/{}/formulations/score` (multi-line decorator), `GET /simulate/cluster` (empty path under a prefix), `POST` and `PUT /simulate/cluster/wake` (`api_route`), and `GET /v1/model/{}/variables` (include_router prefix through a package `__init__`), each `handled_by` its def and `contains`-ed by its file; the label keeps `{project_id}`; the router import binds the variable its file contains; `routes` 11, `routes_unresolved` 4 (factory route, non-literal router prefix, router whose only mount has a non-literal prefix, f-string path), `mounts` 7, `mounts_unresolved` 2.
- Fails on origin/main (`missing endpoint:GET /healthcheck`), and with only the contracts.cpp change reverted (`endpoint count 8`: the router behind the unplaceable mount mints `GET /dynamic`).
- `python_extractor_test.cpp` `fastapi_router_layouts` (PR #143 review fixtures): an aliased import beside the file's own `router` mints `GET /svc/x` and `GET /svc/o/other/y` (was `/svc/o/other/x`); `include_router(users.router, prefix="/api/v1")` mints `GET /api/v1/users/{}` (was `/users/{}`); `app.mount("/api", api)` mints `GET /api/items` (was `/items`) and `session.mount("https://", adapter)` is no mount; a router re-exported through a package `__init__.py` mounted with `prefix="/v2"` mints `GET /v2/items/{}` (was `/items/{}`); `.get` decorators on `cachetools.cache`, a Flask app and `pytest.mark` mint nothing. Each of the four fails on the first commit of this change. A `session.mount("https://", adapter)` inside a function is no mount either (`mounts_unresolved 2` before). `file_extraction_test.cpp` asserts the new Python stub id.
- `graph_builder_test.cpp` `check_shared_stub_alias_binds_both_names`: with `import { router as usersRouter }` in a.ts and `import { router }` in b.ts, both `GET /a/u` and `GET /b/u` exist, both mounts, and both `C extends B0` / `D extends Base` inherits edges, as on bin-v0.6.7.

## Measured on the probe repositories

- ml-backend: `route_resolution` routes 40, routes_unresolved 0, mounts 25, mounts_unresolved 0, endpoints 40 (was all 0); every one of the 40 `@<router>.<verb>(` decorators in the repo mints one endpoint, 10 spot-checked against their prefixes and mounts.
- Turing score: HTTP links 12 -> 13 of 25 (T20 now links); provider_node 16 -> 21 (T16-T20). T16-T19 have their provider endpoint and wait on the consumer side (wrapper `mlBackendRequest` resolution).
- New cross-service seam rows: 1 (`GET /healthcheck` turing-api `health.ts` -> ml-backend `api/app.py`), correct; lost rows 0 in Turing and ModSquad. No other probe repository's graph changed (node and edge sets identical), including after the Python alias-only binding: the graph.json links and nodes of turing-webapp, turing-api, turing-agents, idp, idp-front-end, passless-app and passless-cli are identical to bin-v0.6.7's.
- ml-backend's endpoint set is unchanged by the review fixes; 14 `imports` edges that stopped at a package `__init__.py` now reach the submodule or re-exported symbol they name (for example `from ops.services import sim_cluster` -> ops/services/sim_cluster.py, `from ops.services.webapp_db import ping` -> webapp_db/client.py `ping`), 25 edges in all.

## Non-goals

- Flask and Blueprints: `register_blueprint(bp, url_prefix=)` replaces the blueprint's own prefix, which the mount walk does not express.
- App factories (`def create_app(): app = FastAPI(); app.include_router(...)`, or a module-level `app = create_app()`) and a mount on a router imported from another file (`api_router.include_router(users.router)`): counted unresolved, and a router mounted only there mints nothing.
- `app.add_api_route(...)`: not read. A mount of a longer dotted name (`include_router(api.v1.router)`): counted unresolved, and its router is not identified, so if nothing else mounts it, it is served at its own prefix like any unmounted router.
- Python HTTP clients (`requests`/`httpx`) as consumers.

## Impact

- `src/engine/python_extractor.cpp`: router variables, route decorators, include_router and mount mounts, import alias on the edge, symbol stub id.
- `src/engine/contracts.cpp`: a mount with no mounting chain is counted and leaves an otherwise unmounted child without a path; a Python `module.router` resolves through the imported module.
- `src/engine/index_persistence.cpp`: `kIndexVersionKey` `logic-10`.
- `src/engine/graph_builder.cpp`: alias-only binding; Python package re-exports and submodules in `resolve_imports`.
- `tests/smoke/python_extractor_test.cpp`, `tests/smoke/file_extraction_test.cpp`.
- Graph output for Python repositories gains `variable`, `endpoint` nodes and `contains`, `handled_by`, `mounts` edges; a `from m import router` import now targets the router variable instead of its file. `kIndexVersionKey` is bumped to `logic-10` (#142, which lands first, takes `logic-9`).
