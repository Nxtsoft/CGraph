## ADDED Requirements

### Requirement: FastAPI routers become endpoints
In Python, a module-level assignment `x = APIRouter(...)` or `x = FastAPI(...)` SHALL be a `variable` node contained by its file, carrying `route_prefix` when `APIRouter` is given a string-literal `prefix`; a function decorated `@x.<verb>(path, ...)` with `verb` in get/post/put/patch/delete/head/options, or `@x.api_route(path, methods=[...])` (GET when `methods` is absent), SHALL be a route registration on chain `x` per method, handled by the function; `x.include_router(y, prefix="/p")` SHALL mount `y` under `x` with the literal prefix. These compose into full paths exactly as the JavaScript chains of "Full paths compose through router mounts" do, with `y` resolved through the file's imports, by alias when imported `as` one, including a package's `__init__.py`. A decorated function inside another function, a path or `methods` that is not a list of plain string literals, and an `APIRouter` whose `prefix` is not a string literal SHALL mint no endpoint and SHALL be counted in `routes_unresolved`. An `include_router` inside a function or with a non-literal `prefix` SHALL be counted in `mounts_unresolved`, and a router whose every mount is of that kind SHALL mint no endpoint (each route counted in `routes_unresolved`) rather than be served at its own prefix.

#### Scenario: ml-backend's router tree composes
- **GIVEN** `app = FastAPI()` with `app.include_router(project_router)` and `app.include_router(legacy_router)` in `api/app.py`, `router = APIRouter()` with `router.include_router(setup_router)` in `api/routes/project/router.py` importing `from api.routes.project.setup.routes import router as setup_router`, `router = APIRouter(prefix="/project")` with `@router.post("/{project_id}/setup")` on `setup` in that module, and `router.include_router(design_router, prefix="/v1/model")` in `api/legacy/__init__.py` over `@design_router.get("/{model_id}/variables")`
- **THEN** `endpoint:POST /project/{}/setup` (label `POST /project/{project_id}/setup`) is `handled_by` `setup` and `endpoint:GET /v1/model/{}/variables` is `handled_by` its function, each with a `contains` edge from its file

#### Scenario: Empty paths and api_route
- **GIVEN** `router = APIRouter(prefix="/simulate/cluster")` with `@router.get("")` and `@router.api_route("/wake", methods=["POST", "PUT"])`
- **THEN** `GET /simulate/cluster`, `POST /simulate/cluster/wake` and `PUT /simulate/cluster/wake` exist

#### Scenario: Unknowable paths are counted, not guessed
- **GIVEN** a `@inner.get("/inside")` inside `def create_app()`, `elsewhere = APIRouter(prefix=settings.PREFIX)` with a route, `dynamic_router` mounted only by `include_router(dynamic_router, prefix=settings.PREFIX)`, and `@design_router.get(f"/{PREFIX}/computed")`
- **THEN** none of them mints an endpoint (no `GET /dynamic`), `routes_unresolved` counts all four, and `mounts_unresolved` counts the factory mount and the non-literal one
