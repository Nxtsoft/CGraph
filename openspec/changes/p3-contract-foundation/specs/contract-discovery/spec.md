## ADDED Requirements

### Requirement: Generic contract facts mint repo-free contract nodes
`resolve_contracts` SHALL read `provides_contract` and `uses_contract` raw relations whose context is `<kind>:<name>`, kind one of `table`, `label`, `header`, `claim`, `env`, and whose `target_label` is, for `table` and `label` only, the database the extractor knows the name lives in (empty when it knows none). It SHALL mint one node per contract id, never passed through `make_id`: `table:<database>:<name>` and `label:<database>:<name>` with database `local` when none is known, `header:<name lowercased>`, `claim:<name>`, `env:<name>`. The node's kind SHALL be the contract kind, its label the name as a provider spells it (a user's spelling when this repo has no provider, then with `served: false` and no source anchor), with properties `name` and, for tables and labels, `database`. Each provider SHALL get `handled_by` from the contract and `contains` from the provider's file; each user SHALL get `CONSUMES` to the contract. A fact with an unknown kind, no `:`, an empty name, a database spelled with `:`, or a source no node names SHALL mint nothing and be counted. `route_resolution` SHALL report `contract_facts`, `contract_facts_unresolved`, `contracts_provided`, `contracts_external` and `contract_consumes`, through both stats JSON functions. These raw relations SHALL never become code-graph edges, and nodes of these kinds SHALL never be merged by semantic dedup.

#### Scenario: A table, a header and used-only contracts
- **GIVEN** `createUsers` providing `table:users`, `readTenant` providing `header:X-Tenant-Id`, `listUsers` using `table:users`, `sendTenant` using `header:x-tenant-id`, `claim:org_id` and `env:ML_BACKEND_URL`
- **THEN** `table:local:users` (kind `table`, database `local`) is `handled_by` `createUsers`, contained by its file and consumed by `listUsers`; `header:x-tenant-id` is labelled `X-Tenant-Id`, `handled_by` `readTenant` and consumed by `sendTenant`; `claim:org_id` and `env:ML_BACKEND_URL` are `served: false` with no anchor

#### Scenario: A malformed fact is counted, not minted
- **GIVEN** facts `queue:jobs`, `users`, `claim:`, a table whose database is `a:b`, and a header fact whose source names no node
- **THEN** no node is minted for them and `contract_facts_unresolved` is 5

#### Scenario: A contract fact is never a code-graph edge
- **GIVEN** a `uses_contract` fact whose `target_label` names a type the source file imports
- **THEN** `resolve_raw_relations` adds no edge

#### Scenario: Near-identical contract names stay apart
- **GIVEN** `table:formulation_values` and `table:formulation_value` anchored at the same migration lines, and likewise near-identical labels, headers, claims and env names
- **THEN** semantic dedup merges none of them

### Requirement: One predicate says which contract ids cross repositories
`is_bridged_contract(id)` SHALL be true for every `endpoint:`, `header:`, `claim:` and `env:` id and for a `table:` or `label:` id in a named database, and false for `table:local:` and `label:local:` ids and for every non-contract id. Seam discovery and fuse, workspace `impact` and `path`, and change context's `cross_service` SHALL use it in place of any `endpoint:` prefix test. Proxy prefixes remain endpoint-only.

#### Scenario: Repo-local tables do not cross
- **THEN** `is_bridged_contract("table:local:users")` is false, `is_bridged_contract("table:turing:orders")` and `is_bridged_contract("header:x-tenant-id")` are true
