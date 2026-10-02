## ADDED Requirements

### Requirement: Seam discovery and fuse join every bridged contract, and declared databases and env
`seam discover` SHALL join every contract `is_bridged_contract` accepts the way it joins endpoints, keeping the contract's kind: `SERVED_BY`, `HANDLED_BY`, `CONSUMES` and `CONSUMED_AT`, `served: false` while no graph provides it. A `table:local:` or `label:local:` contract SHALL be left out unless its graph's repo is declared in a database; `--database NAME=repoA,repoB` (and a workspace manifest's `databases`) SHALL spell such a repo's `table:local:<name>` as `table:<NAME>:<name>`, so members of one database meet and no other repo does. `--env VAR=service` SHALL make `env:VAR` served by that service. The endpoint log lines SHALL be unchanged; other contracts SHALL be logged on their own line when there are any, and each declared database with the count of repo-local ids it joined. `seam fuse` SHALL share every bridged id across services, scope every other id (`table:local:` included) to its service, and with `--database` rename a declared member's repo-local table to the database's id. A declaration naming a repo that is not among the `--graph`s, a repo in two databases, or a name declared twice SHALL exit 2; both commands SHALL accept both flags.

#### Scenario: A header joins without a declaration
- **GIVEN** api's graph with `header:x-tenant-id` `handled_by` `readTenant` (labelled `X-Tenant-Id`) and ml's with `sendTenant` consuming `header:x-tenant-id`
- **THEN** discover emits `header:x-tenant-id` (kind `header`, label `X-Tenant-Id`, not `served: false`) `HANDLED_BY` api's `readTenant`, `CONSUMED_AT` ml's `sendTenant`, `SERVED_BY` `service:api`; fuse keeps the one shared id

#### Scenario: Tables join only inside a declared database
- **GIVEN** api providing and ml and billing using `table:local:users`
- **WHEN** discover runs with no declaration
- **THEN** no `table:` node is in the seam, and fuse keeps `api::table:local:users` and `ml::table:local:users`
- **WHEN** discover and fuse run with `--database turing=api,ml`
- **THEN** `table:turing:users` is `HANDLED_BY` api's `createUsers` and `CONSUMED_AT` ml's `listUsers`, never billing's; the log says `database turing (api,ml): 2 repo-local tables and labels joined at the database's id`; fuse gives ml's `listUsers` a `CONSUMES` edge to `table:turing:users` while billing keeps `billing::table:local:users`

#### Scenario: A declared env variable is served by its service
- **GIVEN** ml using `env:API_URL` and `--env API_URL=api`
- **THEN** `env:API_URL` is `SERVED_BY` `service:api` and not `served: false`
