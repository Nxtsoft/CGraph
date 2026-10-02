# Contracts other than HTTP endpoints can join repositories (Phase 3.0)

## Why

Every cross-repo matcher was hardcoded to `endpoint:` ids: seam fuse's shared ids, seam discover's node filter, workspace impact and path, change context's `cross_service`, and the pre-edit file crossings. SQL tables were `make_id`'d (`sql_table_x`), so no other contract could join two repositories. The probe ground truth (`~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/`) has 25 non-HTTP edges that need it: tables and graph labels (T28-T35, M39, M40), headers (T38, T39, T41, M33), JWT claims (M30-M32, T40) and env base URLs (T36, T37, M36-M38). Four later tracks add the extractors for those; this change is the foundation they share, so each of them only adds facts.

## What Changes

- **Generic contract facts.** `provides_contract` / `uses_contract` raw relations, context `<kind>:<name>`, kind `table`, `label` (graph node label or relationship type), `header`, `claim` or `env`; for `table` and `label` the fact's `target_label` is the database the extractor knows (usually none). `resolve_contracts` mints one raw-id node per contract (never `make_id`'d, like `endpoint:` ids, so two repos' graphs hold the same id by construction): kind = the contract kind, label = a provider's spelling, `served: false` when the repo only uses it, `handled_by` to providers, `CONSUMES` from users, `contains` from a provider's file. Ids: `table:<db>:<name>`, `label:<db>:<name>`, `header:<lowercased name>`, `claim:<name>`, `env:<name>`. Five tallies join `route_resolution`. The two relations are on `graph_builder`'s skip list and the five kinds are exempt from dedup.
- **One bridging predicate.** `is_bridged_contract(id)` (contracts.hpp) replaces every `endpoint:` test in seam, workspace, change context and the client. Proxy prefixes stay endpoint-only. JSON keys that say `endpoint` keep working: workspace `impact`'s `bridged` entries add a `contract` key and keep `endpoint` for endpoints.
- **Declarations.** `cgraph.workspace.json` takes `databases: [{"name", "repos"}]` and `env: [{"name", "service"}]`; `seam discover` and `seam fuse` take `--database NAME=repoA,repoB` and `--env VAR=service`. Malformed entries, unknown repos, a repo in two databases and duplicate names are manifest errors (seam commands exit 2), never exceptions.

## Design: database scoping

No code proves which repositories share a database (turing-api reads `DATABASE_URL`, ml-backend `WEBAPP_DB_URL_*`; only comments link them), so the same-database guard is declared, like #144's proxy prefixes.

- A single-repo graph never depends on a workspace. A table or label whose fact names no database is `table:local:<name>` / `label:local:<name>`. `local` is reserved (a declaration may not use it), and `is_bridged_contract` is false for these ids, so two repos' `table:local:users` are never the same contract: seam discover leaves them out, seam fuse scopes them to their service (`ml::table:local:users`), workspace impact and path do not cross at them, and change context lists them `local: true` and asks nobody.
- The declaration is applied where repos are matched, never at extraction: a member declared in database `D` has its `table:local:<name>` spelled `table:D:<name>` there (`declared_contract_id`), and a crossing at `table:D:<name>` is asked of each other member of `D` under its own `table:local:<name>` (`local_contract_spelling`). This is the proxy-prefix pattern: one spelling per repo graph, a shared spelling at the crossing. The alternative, passing the database into extraction through a per-repo config file, would make a graph depend on which workspace it sits in and need a rebuild whenever the manifest changes.
- A repo may be declared in at most one database: its `table:local:` ids could not say which one they mean.
- A fact that names a database (`target_label`) mints `table:<that db>:<name>` directly, which bridges by construction; that is a declaration made by the extractor, and the scope must then be a database identity, not a connection variable.
- An env name is no evidence by itself (every service reads `NODE_ENV`), so `env:<NAME>` crosses repositories only when declared, as plan 3.4 says ("declared only"): the manifest's `env` / `--env` names the member it addresses, and an undeclared env id stays in its repo like a `table:local:` one (scoped per service in fuse, never crossed). Seam discover adds `SERVED_BY service:<member>`; workspace impact lists a reached env id in `bridged` with `provided_by`; change context gives a used env id one `provider` row of kind `service`.

## Contract that tests verify

- `contracts_test.cpp` `test_generic_contract_facts`: ids, kinds, labels (provider spelling, header case-folded), `database`, `served: false`, `handled_by` / `CONSUMES` / `contains`, five malformed facts minted nothing, tallies through `contract_resolution_json`, `is_bridged_contract`.
- `contract_declarations_test.cpp`: database spelling both ways, env provider, parsing, flags, round trip, every malformed or conflicting declaration an error.
- `seam_test.cpp` `test_generic_contracts`: graphs built with `resolve_contracts` from raw facts; a header joins with no declaration, an undeclared table does not enter the seam, a declared database joins its members only (log line counted), a declared env is served by its service, fuse shares the bridged ids and renames declared tables.
- `workspace_test.cpp`: manifest `databases` / `env` load, round-trip, ten malformed or conflicting manifests are errors; impact and path cross at a header both ways; a table crosses only inside its declared database and not without one; an env reached names its provider; the existing endpoint `bridged` entry also carries `contract`.
- `change_context_test.cpp` `test_contract_crossings`: `touch_contracts` on a graph from raw facts; `cross_service_section` asks a declared member under its own spelling and never an outsider, keeps contract nodes out of rows, adds the env provider row, and lists an undeclared table `local`.
- `graph_builder_test.cpp` `test_contract_facts_are_not_edges`, `dedup_test.cpp` near-identical contract names, `operation_stats_test.cpp` tally round trip, `index_persistence_test.cpp` `logic-14`.

## Non-goals

- No extractor emits the new facts (3.1-3.4 do), so the probe scores for tables, headers, claims and env are unchanged.
- Seeding `impact` at a database's spelling (`table:turing:users`), which no single graph holds, is not federated; seed at a member's own id.
- Proxy prefixes for non-endpoint contracts.
- `seam generate` (hand-written specs) is unchanged.

## Impact

- New: `src/engine/contract_declarations.cpp` (+ header, test).
- `src/engine/contracts.cpp`, `contracts.hpp`, `operation_stats.*`, `graph_builder.cpp`, `dedup.cpp`, `index_persistence.cpp` (`logic-14`).
- `src/engine/seam.cpp`, `seam.hpp`, `src/cli/main.cpp` (`--database`, `--env`).
- `src/engine/workspace.cpp`, `workspace.hpp` (manifest, crossing).
- `src/engine/change_context.cpp`, `change_context.hpp` (`touch_contracts`), `src/client/client_runtime.cpp`.
- Output format: `stats.json` / `status` `route_resolution` gain five keys; workspace `impact` `bridged` entries gain `contract`. Graphs of repositories without the new facts are unchanged.
