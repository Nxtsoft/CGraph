## ADDED Requirements

### Requirement: FastAPI routers become endpoints
In Python, a module-level assignment `x = APIRouter(...)` or `x = FastAPI(...)` SHALL be a `variable` node contained by its file, carrying `route_prefix` when `APIRouter` is given a string-literal `prefix`; a function decorated `@x.<verb>(path, ...)` with `verb` in get/post/put/patch/delete/head/options, or `@x.api_route(path, methods=[...])` (GET when `methods` is absent), SHALL be a route registration on chain `x` per method, handled by the function; `x.include_router(y, prefix="/p")` and `x.mount("/p", y)` SHALL mount `y` under `x` with the literal prefix (a `mount` whose path does not start with `/` or whose app is not a name SHALL NOT be a mount). These compose into full paths exactly as the JavaScript chains of "Full paths compose through router mounts" do, with `y` resolved through the file's imports: an import `as` an alias SHALL bind only the alias, a name a package's `__init__.py` imports SHALL be importable from the package as what it names, a name the package neither declares nor re-exports SHALL be its submodule, and `y` written `module.router` SHALL be the `router` variable of the imported module `module`, and a `y` bound by an enclosing `for y in (a, b):` over a literal tuple or list SHALL be each of its items. A decorated function inside another function, a path or `methods` that is not a list of plain string literals, and an `APIRouter` whose `prefix` is not a string literal SHALL mint no endpoint and SHALL be counted in `routes_unresolved`. An `include_router` inside a function, with a non-literal `prefix`, or on anything but a module-level router of its file (`app = create_app()`, a router imported from another file, `app.router`, `self.app`) SHALL be counted in `mounts_unresolved`, and a router whose every mount is of that kind SHALL mint no endpoint (each route counted in `routes_unresolved`) rather than be served at its own prefix.

#### Scenario: ml-backend's router tree composes
- **GIVEN** `app = FastAPI()` with `app.include_router(project_router)` and `app.include_router(legacy_router)` in `api/app.py`, `router = APIRouter()` with `router.include_router(setup_router)` in `api/routes/project/router.py` importing `from api.routes.project.setup.routes import router as setup_router`, `router = APIRouter(prefix="/project")` with `@router.post("/{project_id}/setup")` on `setup` in that module, and `router.include_router(design_router, prefix="/v1/model")` in `api/legacy/__init__.py` over `@design_router.get("/{model_id}/variables")`
- **THEN** `endpoint:POST /project/{}/setup` (label `POST /project/{project_id}/setup`) is `handled_by` `setup` and `endpoint:GET /v1/model/{}/variables` is `handled_by` its function, each with a `contains` edge from its file

#### Scenario: Empty paths and api_route
- **GIVEN** `router = APIRouter(prefix="/simulate/cluster")` with `@router.get("")` and `@router.api_route("/wake", methods=["POST", "PUT"])`
- **THEN** `GET /simulate/cluster`, `POST /simulate/cluster/wake` and `PUT /simulate/cluster/wake` exist

#### Scenario: Unknowable paths are counted, not guessed
- **GIVEN** a `@inner.get("/inside")` inside `def create_app()`, `elsewhere = APIRouter(prefix=settings.PREFIX)` with a route, `dynamic_router` mounted only by `include_router(dynamic_router, prefix=settings.PREFIX)`, and `@design_router.get(f"/{PREFIX}/computed")`
- **THEN** none of them mints an endpoint (no `GET /dynamic`), `routes_unresolved` counts all four, and `mounts_unresolved` counts the factory mount and the non-literal one

#### Scenario: An alias shadows nothing
- **GIVEN** `pkg/svc.py` with `from pkg.other import router as other_router`, its own `router = APIRouter(prefix="/svc")`, `router.include_router(other_router, prefix="/o")` and `@router.get("/x")`, and `pkg/other.py` with `router = APIRouter(prefix="/other")` and `@router.get("/y")`
- **THEN** exactly `GET /svc/x` and `GET /svc/o/other/y` exist

#### Scenario: Submodules, package re-exports and sub-applications
- **GIVEN** `from app.routers import users` with `app.include_router(users.router, prefix="/api/v1")` over `APIRouter(prefix="/users")` with `@router.get("/{user_id}")`; `api/routes/__init__.py` importing `from api.routes.items import router as items_router` and `main.py` with `from api.routes import items_router` and `app.include_router(items_router, prefix="/v2")` over `APIRouter(prefix="/items")` with `@router.get("/{item_id}")`; and `app.mount("/api", api)` of `api = FastAPI()` with `@api.get("/items")`
- **THEN** `GET /api/v1/users/{}`, `GET /v2/items/{}` and `GET /api/items` exist, and none of them at the router's own top-level path

#### Scenario: A mount nobody can place hides its router
- **GIVEN** `app = create_app()` with `app.include_router(users.router, prefix="/api/v1")`, and `app/wire.py` importing `api_router` from `app/api.py` with `api_router.include_router(users.router)` and `api_router.include_router(items.router)` after `import app.routers.items as items`
- **THEN** no `GET /users/{}` or `GET /items/{}` exists, and each such mount counts in `mounts_unresolved`

#### Scenario: Attribute mounts and literal loops
- **GIVEN** `app.router.include_router(users.router, prefix="/api")`, and separately `self.app.include_router(users.router, prefix="/api")` in a method, over `APIRouter(prefix="/users")` with `@router.get("/{x_id}")`; and `for r in (users.router, items.router): app.include_router(r, prefix="/api/v1")` on `app = FastAPI()`
- **THEN** the first two mint nothing and each counts in `mounts_unresolved`, and the loop mints `GET /api/v1/users/{}` and `GET /api/v1/items/{}`
