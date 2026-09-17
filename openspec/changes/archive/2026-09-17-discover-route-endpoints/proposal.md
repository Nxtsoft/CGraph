# Feature: endpoint nodes discovered from route registrations (CGR-13, slice 1)

## Why

CGraph's one multi-repo primitive, the cross-service seam, joins services through hand-written
contracts. The field test in the next-steps plan wrote one seam by hand for turing-api and
turing-webapp and measured what that covered: **1 of 209** route definitions, after grepping both
repos and walking both `graph.json` files to find anchor lines. The design record says why
(`openspec/changes/archive/2026-06-18-generate-cross-service-seam/design.md`): "The spec is
hand-written today... a spec-discovery helper is a separate future capability, explicitly a
non-goal." This change is that capability's first slice: the provider side. The routes a service
serves are in its source, in a handful of framework shapes, and the graph already names every
inline handler (`notebookRoutes.get /starred-notes`, CGR-4). What it lacks is the full URL, which
lives across files: turing-api's `notebookRoutes` carries `prefix: '/notebooks'`, is mounted by
`notebooksModule` (cast `as unknown as Elysia` to dodge TS2589), which `apiRoutes` mounts under
`/api/v1`, which `app` mounts `as any`. No file states `GET /api/v1/notebooks/starred-notes`.

## What Changes

- **Route facts at extraction** (`javascript_extractor.cpp`). Two new `RawRelation` kinds:
  `route` (source = the inline handler's node, target = the chain identifier it is registered
  on, context = `<verb> <path>` as written) emitted by the relation handler for every handler
  `route_registration` recognises; and `mounts` (source = the mounting chain's variable node,
  target = the mounted identifier, context = the mount path or empty) emitted by the extra walk
  for `.use(x)`, `.use('/p', x)` and `.route('/p', x)`. A chain variable's own prefix
  (`new Elysia({ prefix })`, `.basePath()`) becomes its `route_prefix` property. Chain roots and
  mount arguments read through `as`, `satisfies`, `!`, parentheses and `<T>` casts, which also
  fixes the handler names of chains ending in such casts. A chain passed inline to an enclosing
  chain's `.use()` / `.route('/p', …)`, and the parameter of a `.group('/p', app => …)` /
  `.guard(opts, app => …)` callback, root at the enclosing chain with the path beneath it (the
  handler is then labelled `apiRoutes.get /v2/x`); the parameter of any other function leaves the
  chain empty, which resolution counts rather than guesses. A variable whose value is another
  identifier (`export const deckModule = deckRoutes as unknown as Elysia`) records an `aliases`
  fact. An aliased import (`import { config as configModule }`) keeps its alias on the import
  stub, `resolve_imports` carries it onto the relinked edge, and the shared scope index binds
  both names. An exported `GET`/`POST`/... at the top of a Next.js `app/**/route.ts` records a
  `file_route` fact with the path its file serves (`[id]` to `:id`, `[...slug]` to `*`, `(group)`
  and `@slot` dropped).
- **Endpoint minting after merge** (`contracts.cpp`, new; `resolve_contracts`). Mount facts
  resolve their target through the file's imports, then its own declarations, to a `variable`
  node, and become `mounts` edges (with a `prefix` property when the mount has a path). Every
  route's chain resolves the same way, and its full paths are its own prefix beneath every path
  of every parent, recursively, to the top-level chains; a chain nobody mounts is top-level, a
  cycle stops at the revisit. Each (method, full path) mints one `endpoint` node: id
  `endpoint:GET /api/v1/notebooks/starred-notes`, label the same without the prefix, kind
  `endpoint`, properties `method` and `path`, the handler's `source_file` and `source_location`,
  a `contains` edge from the handler's file, and a `handled_by` edge to the handler so
  `graph_impact` on the handler (dependents) reaches its endpoint. The id carries no repo, so
  the consumer side of a later slice joins by construction. An alias is a mount with no path and
  no edge. A route whose chain the file neither declares nor imports, or that the extractor could
  not root, is refused and counted rather than minted at a wrong path. The pass runs in
  `run_one_shot` and in the
  incremental `rebuild_graph`, both of which re-merge every fragment, so a prefix edit in
  `app.ts` re-paths every endpoint on the next update.
- **Dedup exemption** (`dedup.cpp`). `endpoint` nodes never enter fuzzy dedup: sibling routes are
  deliberately near-identical strings (`GET /notebooks/:id/notes` beside `.../votes`).
- **Tally** (`stats.json` `route_resolution`: routes, routes_unresolved, mounts,
  mounts_unresolved, endpoints), since an unresolved route leaves no node to notice.
- Shared scope index: the per-file declared/imported name maps `resolve_raw_relations` built
  inline are now `build_relation_scopes` / `resolve_scoped_name` in `graph_builder`, used by
  both resolvers so a name binds the same way in each.
- Docs: skill routing row for endpoint questions, README, CLAUDE.md pipeline note.

### Non-goals
- Consumers (`fetch`/`apiFetch` call sites → `CONSUMES` edges) and `seam discover`: slice 2.
- Contract files (OpenAPI, protobuf, GraphQL) as `schema` nodes: slice 3.
- Repo-scoped node ids (`repo:<name>`): separate, behind a flag with regenerated goldens.
- Handlers passed by name (`app.get('/x', handler)`): the route fact needs the inline handler
  node the walker creates; a named handler is a later extension of `route_registration`.
- Routers received as function parameters (`export function register(app) { app.get(…) }`):
  their mount is in the caller, so they are refused and counted.
- Shadowing inside nested blocks: a `const app` declared directly in an enclosing function body
  is recognised as local (and refused); one declared inside an `if` or `for` block within that
  body is not searched and would bind to a module-level `app` of the same name.
- Python (FastAPI/Flask), Go (`mux.HandleFunc`) and Java (Spring) route shapes.

## Impact

- **Touches:** `src/engine/include/cgraph/contracts.hpp` (new), `src/engine/contracts.cpp`
  (new), `src/engine/javascript_extractor.cpp`, `src/engine/graph_builder.cpp` and its header,
  `src/engine/include/cgraph/language_config.hpp` (`ExtraWalk` gains a `RawRelation` sink;
  `cpp_field_walk`, `go_extra_walk`, `rust_extra_walk` take and ignore it), `extractor.cpp`,
  `dedup.cpp`, `pipeline.cpp`, `incremental_update.cpp`, `report.cpp` (reuses
  `next_route_path`), `operation_stats.hpp/.cpp`, CMake lists, `tests/smoke/contracts_test.cpp`
  (new), `javascript_extractor_test.cpp`, `dedup_test.cpp`, docs.
- **Parity.** A repo with no router chains gains no node, edge or property; CGraph's own graph
  is unchanged. A repo with routes gains `endpoint` nodes, `handled_by`/`mounts`/`contains`
  edges and `route_prefix` properties in `graph.json`, as `field` nodes did in CGR-9; the
  extraction fragment shape and id normalization are untouched.
- **Measured** on the field-test pair at their remote heads (numbers in tasks.md 3.3-3.4).

## Capabilities

### New Capabilities
- `contract-discovery` — the wire contracts a repo provides, found in its own source; this slice: HTTP endpoints.

### Modified Capabilities
- `deterministic-graph-pipeline` — endpoint resolution runs in every build path, endpoints are exempt from fuzzy dedup, and `stats.json` reports the tally.
