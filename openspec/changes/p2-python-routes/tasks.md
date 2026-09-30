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
