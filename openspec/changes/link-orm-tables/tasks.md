## 1. Tests first

- [x] 1.1 `contracts_test.cpp` `test_orm_table_links`: real SQL and TypeScript extractors; literal pgTable / mysqlTable names map, including a quoted mixed-case name through `as`; unmigrated, non-literal and non-Drizzle names do not.
- [x] 1.2 `daemon_ops_test.cpp`: `trace_impact` from a table reaches its model, the handler file and the endpoint the file contains, not a function it contains, stops at a `service` hub unless the hub is the seed still walks through the model's own file to a barrel's importers, does not serve a file's other routes, or a mounting app file's routes, from a function or endpoint climb, serves a seeded file's endpoints, and gives the same depths in either seed order.

## 2. Implementation

- [x] 2.1 `javascript_extractor.cpp`: record `maps_table` raw relations for Drizzle table declarations.
- [x] 2.2 `contracts.cpp`: resolve them to `maps_table` edges; `graph_builder.cpp` skips them in `resolve_raw_relations`.
- [x] 2.3 `daemon_ops.cpp` `trace_impact`: file-to-endpoint propagation on dependents walks, except for files reached only through a climb from a function or endpoint (a strong/weak walk, order-independent); `service` nodes reported but not expanded past the seed.
- [x] 2.4 `index_persistence.cpp`: bump `kIndexVersionKey` to `logic-7` (graph output changes).

## 3. Verification

- [x] 3.1 Mutation: removing each piece fails its test.
- [x] 3.2 Full suite (`ctest --test-dir build/default`), unchanged apart from the known mars-only `cgraph_file_watcher_test` failure.
- [x] 3.3 Real flow: area 7's harness on `~/swarm/turing-api` at `4cf461e`, main (bin-v0.6.3) vs this branch, through `cgraph seam query ... impact`.
- [x] 3.4 `scripts/change_impact_benchmark.py`: main vs this branch, identical summary for all three arms.
- [x] 3.5 Function seeds: 30 route-file functions on turing-api report the same 114 nodes and 38 endpoints as main.
- [x] 3.6 `openspec validate link-orm-tables --strict`.
