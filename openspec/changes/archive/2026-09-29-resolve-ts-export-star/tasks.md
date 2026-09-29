## 1. Tests first

- [x] 1.1 `graph_builder_test.cpp` `check_typescript_export_star`, through `run_one_shot`: nested star barrels, an aliased named re-export, two barrels starring one module, and a star cycle.

## 2. Implementation

- [x] 2.1 `javascript_extractor.cpp`: mark `re_exports` edges with `star=true` for a bare `export *`, and `reexport=<exported name>` for named re-exports.
- [x] 2.2 `graph_builder.cpp`: build the re-export maps from edges (per owner file, several owners per stub), keep the Rust node tag, and add a bounded breadth-first follow through star targets.

## 3. Verification

- [x] 3.1 Red: the new test fails on origin/main's resolver and extractor.
- [x] 3.2 Mutation: without the star follow, and without the alias key, the test fails.
- [x] 3.3 Full suite (`ctest --test-dir build/default -j4`), including the Rust re-export tests, unchanged.
- [x] 3.4 Real flow: build `~/swarm/turing-api` with this branch's CLI. Count service files whose `imports` edges now reach Drizzle table declarations, and check that `impact(dependents)` from a table object reaches its services.
- [x] 3.5 `openspec validate resolve-ts-export-star --strict`.
