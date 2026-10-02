## 1. Generic contract facts

- [x] 1.1 `contracts.hpp` fact table: `provides_contract` / `uses_contract`, context `<kind>:<name>`, database in `target_label` for `table` / `label`; id helpers `contract_id`, `contract_kind_of`, `is_bridged_contract`, `is_database_local_contract`.
- [x] 1.2 `resolve_contracts` step 8 mints raw-id contract nodes with `handled_by`, `CONSUMES`, `contains`, `served: false`; five new `route_resolution` tallies in both stats JSON functions.
- [x] 1.3 `graph_builder.cpp` skips the two relations; `dedup.cpp` exempts the five kinds.
- [x] 1.4 Index version `logic-14`.

## 2. One bridging predicate

- [x] 2.1 `seam.cpp`: fuse `shared_id`, discover's contract filter and log lines.
- [x] 2.2 `workspace.cpp`: `shared_contract`, `spellings_in`, `is_placeholder`, impact `bridged` (`contract` key beside `endpoint`), `reachable_contracts`.
- [x] 2.3 `change_context.cpp`: `served_contracts`, `touch_contracts` (exported), `cross_service` rows; `client_runtime.cpp` file crossings.
- [x] 2.4 Proxy prefixes stay endpoint-only (`endpoint_prefixes.cpp` unchanged).

## 3. Declarations

- [x] 3.1 `contract_declarations.cpp`: parse, flags, validation, JSON round trip, database spelling both ways, env provider.
- [x] 3.2 Manifest `databases` / `env` in `load_workspace` and `workspace_manifest_json`.
- [x] 3.3 `seam discover` / `seam fuse` take `--database NAME=a,b` and `--env VAR=service`; bad declarations exit 2.

## 4. Verification

- [x] 4.1 Tests: `contracts_test`, `contract_declarations_test`, `seam_test`, `workspace_test`, `change_context_test`, `graph_builder_test`, `dedup_test`, `operation_stats_test`, `index_persistence_test`.
- [x] 4.2 Fail-before / pass-after with the `src` diff reverse-applied.
- [x] 4.3 Probe identity: eight graphs, seam discover and fuse for both systems, against bin-v0.7.2.
- [x] 4.4 Scorer baseline with T12 / M34 / M35 scored as HTTP.

## 5. Review round 1 (PR #158)

- [x] 5.1 `env:` ids cross only when declared; standard HTTP headers (IANA permanent field names, retrieved 2026-10-01, plus common tracing and proxy headers) never cross; `seam fuse` uses `--env`.
- [x] 5.2 One crossing rule (`crossing_id`, `contract_spellings`) shared by seam, workspace and change context; a home table that names its database reaches members under their local spelling and the reverse.
- [x] 5.3 Change context partitions repo-local contracts out before the 24-contract cap.
- [x] 5.4 `trace_impact` serves every contract kind from its file, so a changed file touches its tables and headers.
- [x] 5.5 The three endpoint-only requirements are MODIFIED with every existing scenario; fused declared tables carry their database; `contract_id` refuses a database `local`; extractor normalization documented; per-kind summary wording; `declared_database` reused.
