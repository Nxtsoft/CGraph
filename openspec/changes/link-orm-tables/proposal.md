# Impact from a SQL table reaches its model, handlers and endpoints

## Why

CGraph extracts a `sql_table` node for every `CREATE TABLE` in a repo's migrations, but nothing connects it to the code. On turing-api, `impact` from any migration table reaches only other tables through foreign keys: none of the 175 Drizzle `pgTable(...)` models, and none of the handlers that use them. Two more gaps stop the walk. A dependents walk that reaches a handler file never continues to the endpoints that file serves, because the file `contains` the endpoint and the walk only follows edges backwards. And in a fused seam graph, every endpoint is `SERVED_BY` its service node, so one stray edge into that hub reaches every endpoint and consumer of the service.

The area 7 measurement (`~/.agents/artifacts/2026-09-25/cgraph-context-engine-research/briefs/area7-data-seams/results.md`) prototyped these as rules R1 (SQL-to-ORM link) and R3 (file to endpoint), plus the hub. It found R1 needs the TypeScript barrel resolution that shipped in #126 (bin-v0.6.3) to be useful at engine defaults. This change is the engine version of R1, R3 and the hub stop.

## What Changes

- The JavaScript/TypeScript extractor records a `maps_table` fact for each module-level `pgTable` / `mysqlTable` / `sqliteTable` declaration with a string-literal name. `resolve_contracts` turns it into a `maps_table` edge from the model variable to the migration's `sql_table` node, when one exists.
- `trace_impact`, which serves `impact` and change-context's dependents, treats an endpoint as a dependent of the file that contains it. It reports a `service` node it reaches but does not walk through it, unless the service is the seed.

## Measured on turing-api

Area 7's harness (40 sampled tables, `cgraph seam query ... impact` from `sql_table_<name>`, dependents) on `~/swarm/turing-api` at `4cf461e`, the commit area 7 pre-registered. Main is bin-v0.6.3, which already has #126's barrel resolution. Handler coverage is the per-table share of ground-truth handler files reached; precision is ground-truth handlers over all route files reached.

| setting | main: handlers / endpoints / precision | this change: handlers / endpoints / precision | route files reached (of 88) |
|---|---|---|---|
| depth 3, limit 200 (defaults) | 0.000 / 0.000 / n/a | 0.951 / 0.049 / 0.802 | 4.6 |
| depth 5 | 0.000 / 0.000 / n/a | 1.000 / 1.000 / 0.059 | 51.0 |
| depth 8 | 0.000 / 0.000 / n/a | 1.000 / 1.000 / 0.032 | 87.0 |

The graph has 12,369 nodes on both sides; this change adds 175 `maps_table` edges.

The historical change-impact benchmark (`scripts/change_impact_benchmark.py`, six pinned Click, Flask and Requests changes, one repetition) was run against main and against a variant that also stopped the walk at a symbol's own file; both scored the same recall (1.000 and 0.667 for the two arms). That variant was dropped anyway: review showed it loses real dependents the benchmark does not cover, such as a module that calls `pkg.base.outer()` through `import pkg.base` (no resolved call edge), a resolved caller of a function enclosing the changed one, and endpoints of a route file that declares its own model.

## Non-goals

- Prisma `@@map`, TypeORM `@Entity('<name>')`, schema-qualified Drizzle tables (`pgSchema('s').table(...)`), and table names built from constants.
- Changing the default impact depth. At depth 3 a table reaches its handler files but usually not their endpoints, which sit one hop further.
- The over-reach at depth 5 and above (51 of 88 route files at depth 5, 87 at depth 8). The walk climbs from a model to its schema file, other schema files import models for foreign keys, and those files are re-exported by the `db/schema` barrels; every file importing anything from a barrel keeps a file-level `imports_from` edge to it (231 at depth 4 from `sql_table_competitors`). Cutting the walk at the file loses real dependents (see above), so the fix belongs in graph construction: drop the file-level edge to a barrel when every name the importer takes from it resolved through the barrel. That is a separate proposal.

## Impact

- `src/engine/javascript_extractor.cpp`: `drizzle_table_name` and the `maps_table` raw relation.
- `src/engine/contracts.cpp`, `include/cgraph/contracts.hpp`: step 7 resolves `maps_table`. `src/engine/graph_builder.cpp`: `resolve_raw_relations` skips it.
- `src/engine/daemon_ops.cpp` `trace_impact`: file-to-endpoint and the service-hub stop.
- `graph.json`: one `maps_table` edge per mapped model (175 on turing-api). No node or id changes. `kIndexVersionKey` is bumped to `logic-7`.
- Tests: `contracts_test.cpp` `test_orm_table_links`, a data-seam block in `daemon_ops_test.cpp`, and the version literal in `index_persistence_test.cpp`.
