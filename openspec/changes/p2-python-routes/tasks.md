## 1. Extraction

- [x] 1.1 Module-level `APIRouter(prefix=)` / `FastAPI()` assignments become `variable` nodes with `route_prefix`, contained by their file.
- [x] 1.2 `@x.<verb>(path)` and `@x.api_route(path, methods=[...])` decorated defs emit `route` facts; defs inside functions and non-literal paths/methods are recorded unresolvable.
- [x] 1.3 `x.include_router(y, prefix=)` emits `mounts`; inside a function or with a non-literal prefix it carries no mounting chain.
- [x] 1.4 Aliased `from m import a as b` carries `alias` on the file's `imports` edge.

## 2. Resolution

- [x] 2.1 `resolve_contracts` counts a mount with no mounting chain and leaves a child with no other mount pathless (routes counted unresolved).

## 3. Verification

- [x] 3.1 `python_extractor_test.cpp` `fastapi_routes`; fails on origin/main and with only the contracts change reverted.
- [x] 3.2 Probe: ml-backend 40/40 endpoints; T20 links; T16-T19 provider ids present; no lost seam rows.

## 4. Review of PR #143

- [x] 4.1 An aliased Python import binds only its alias (`build_relation_scopes`); a JavaScript alias, shared on the stub, still binds both names, and the JavaScript/Kotlin/Go probe graphs are identical to bin-v0.6.7.
- [x] 4.2 `module.router` mounts resolve through `from pkg import module` submodule imports.
- [x] 4.3 Routers re-exported through a package `__init__.py` are followed; Python symbol stubs no longer collide under `make_id`.
- [x] 4.4 `app.mount("/p", sub_app)` is a mount; `session.mount("https://", adapter)` is not, inside a function too.
- [x] 4.5 `fastapi_router_layouts` fails on the first commit and passes after; probe re-scored.
