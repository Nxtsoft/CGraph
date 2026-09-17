# contract-discovery Specification

## Purpose
TBD - created by archiving change discover-route-endpoints. Update Purpose after archive.
## Requirements
### Requirement: Route registrations become endpoint nodes
For every inline HTTP route handler the JavaScript/TypeScript extractor names (`<chain>.<verb>('<path>', ..., handler)` with `verb` in get/post/put/patch/delete/head/options/all), the pipeline SHALL mint one `endpoint` node per (method, full path) with id `endpoint:<METHOD> <full path>`, label `<METHOD> <full path>`, kind `endpoint`, properties `method` and `path`, the handler's `source_file` and `source_location`, a `contains` edge from the handler's file node, and a `handled_by` edge from the endpoint to the handler; two handlers registering the same method and path SHALL share one endpoint node with two `handled_by` edges.

#### Scenario: A handler mints its endpoint
- **GIVEN** `const app = new Elysia(); app.get('/health', () => 'ok')` in `src/app.ts`
- **WHEN** the graph is built
- **THEN** a node `endpoint:GET /health` exists with kind `endpoint`, `method` `GET`, `path` `/health`, source file `src/app.ts`, a `contains` edge from the file and a `handled_by` edge to the `app.get /health` handler

#### Scenario: A repository without routers is unchanged
- **GIVEN** a repository whose files register no routes and mount no chains
- **THEN** the graph gains no node, edge or property, and `route_resolution` reports zero routes, mounts and endpoints

### Requirement: Full paths compose through router mounts
The full path of a route SHALL be its chain's own prefix (`new Elysia({ prefix })` option or `.basePath()` link, recorded as the chain variable's `route_prefix` property) joined beneath every path of every chain that mounts it (`.use(child)`, `.use('/p', child)`, `.route('/p', child)`, read through `as`, `satisfies`, `!`, parenthesis and `<T>` casts), recursively to the top-level chains, with mount targets resolved through the mounting file's imports (by alias when imported `as` one) and then its own declarations to a `variable` node; a variable whose value is another identifier (`export const deckModule = deckRoutes as unknown as Elysia`) SHALL compose as a mount with no path and no edge; a chain built inline as the argument of an enclosing chain's `.use()` or `.route('/p', …)`, and the parameter of a `.group('/p', app => …)` or `.guard(opts, app => …)` callback, SHALL root at the enclosing chain with the mount or group path and its own constructor prefix beneath it, and its handlers SHALL be labelled by that chain and composed path; a chain nobody mounts SHALL be top-level; a chain mounted under several paths SHALL mint one endpoint per path; a mount cycle SHALL stop at the revisited chain; paths SHALL join with single slashes, duplicate slashes collapsed and no trailing slash except the root; each resolved mount SHALL become a `mounts` edge from the mounting to the mounted variable carrying a `prefix` property when the mount has a path.

#### Scenario: Three import hops and two casts compose one path
- **GIVEN** `notebookRoutes = new Elysia({ prefix: '/notebooks' }).get('/starred-notes', handler)` in one file, `notebooksModule = new Elysia().use(notebookRoutes) as unknown as Elysia` importing it in a second, and `apiRoutes = new Elysia({ prefix: '/api/v1' }).use(notebooksModule as any)` with `app = new Elysia().use(apiRoutes as any)` importing it in a third
- **THEN** the endpoint is `GET /api/v1/notebooks/starred-notes`, and `mounts` edges run app → apiRoutes → notebooksModule → notebookRoutes

#### Scenario: Express mount paths and a doubly mounted router
- **GIVEN** `router.get('/items', handler)` in one file and `app.use('/api', router); app.use('/legacy', router)` importing it
- **THEN** endpoints `GET /api/items` and `GET /legacy/items` both have `handled_by` edges to the one handler, and the app → router `mounts` edge carries `prefix` `/api`

#### Scenario: A root path under a prefix
- **GIVEN** a chain with prefix `/notebooks` registering `.get('/')`
- **THEN** the endpoint path is `/notebooks`

#### Scenario: Aliased import, inline chain, group callback and cast alias
- **GIVEN** `apiRoutes = new Elysia({ prefix: '/api/v1' }).use(configModule).use(deckModule).use(new Elysia({ prefix: '/inline' }).get('/protected', h)).group('/v2', (app) => app.get('/x', h2))` with `import { config as configModule }` of `config = new Elysia({ prefix: '/config' }).get('/luna', h3)` and `deckModule = deckRoutes as unknown as Elysia` over `deckRoutes = new Elysia({ prefix: '/notebook-docs' }).post('/:id/deck', h4)`
- **THEN** the endpoints are `GET /api/v1/config/luna`, `POST /api/v1/notebook-docs/:id/deck`, `GET /api/v1/inline/protected` and `GET /api/v1/v2/x`, the inline and grouped handlers are labelled `apiRoutes.get /inline/protected` and `apiRoutes.get /v2/x`, and there is no `mounts` edge from `deckModule` to `deckRoutes`

### Requirement: Unresolvable chains are refused and counted
A route whose chain the extractor could not root (a router received as an ordinary function parameter, an unassigned chain in an expression statement) or whose chain identifier resolves to no `variable` node through the file's imports or declarations SHALL mint no endpoint and SHALL be counted in `route_resolution.routes_unresolved`; a mount whose target identifier resolves to nothing SHALL be counted in `mounts_unresolved`; a mount whose target is not an identifier (`.use(cors())`) or resolves to a non-variable (middleware function) SHALL be neither a mount nor an error.

#### Scenario: Parameter and local routers are refused
- **GIVEN** `export const app = new Elysia({ prefix: '/real' }).get('/y', h)`, `export function register(app) { app.get('/x', h2) }` and `describe('r', () => { const app = new Elysia(); app.post('/probe', h3) })` in one file
- **THEN** `GET /real/y` is minted, no endpoint ending in `/x` or `/probe` exists, and `routes_unresolved` is 2

### Requirement: Next.js route files become endpoints
A module-level function named by an uppercase HTTP verb (`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`) in a `route.ts|js|tsx|jsx|mjs` file under an `app` directory SHALL mint an endpoint whose path is the file's path below the last `app` directory with `[name]` segments as `:name`, `[...name]` and `[[...name]]` as `*`, and `(group)` and `@slot` segments dropped; a lowercase `get` in such a file SHALL NOT.

#### Scenario: A dynamic segment route file
- **GIVEN** `export async function GET()` and `export const POST = async () => {}` in `app/api/admin/connectors/[id]/route.ts`
- **THEN** endpoints `GET /api/admin/connectors/:id` and `POST /api/admin/connectors/:id` exist, each `handled_by` its function

