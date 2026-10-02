## MODIFIED Requirements

### Requirement: Change context names the other services a change crosses into
When the target root belongs to a workspace, change context SHALL add a `cross_service` section naming the workspace and home repository, the contracts the change touches, and rows for the other repositories. Contracts are every node `contract_kind_of` names: endpoints, tables, graph labels, headers, claims and env names. A change SHALL serve a contract it edits, one whose provider it reaches through `handled_by`, or one a changed file directly contains (a file seed reaches the contracts it contains whatever their kind); it SHALL remove a contract served in the base snapshot and not the target, and add one served in the target and not the base, marking such a contract `outside_diff` and ranking it after every other when the roots also differ in files the diff does not supply; it SHALL use a contract that changed code consumes, or that a function calling a changed helper consumes. Only a contract that crosses repositories (`crossing_id`: as for workspace `impact`, with the home repository's declared database and env) SHALL be ranked (edited, removed or added first) and at most 24 asked, in rank order; every other touched contract SHALL be listed after them with `local: true`, asked of nobody and not counted against the 24. A contract SHALL be asked of each other repository under every spelling it may hold it under (`contract_spellings`): the crossing id, and inside the same declared database the repository's own `table:local:` id, so a home table that names its database and one that does not reach the same members; a crossing id differing from the home id SHALL be listed as `shared_id`. For each served, removed or added contract the other repositories' direct `CONSUMES` users SHALL be rows with `relation: consumer`; for each used contract their `handled_by` provider SHALL be a row with `relation: provider`, and a used env variable declared to address another member SHALL yield one `provider` row of kind `service` for that member; contract nodes SHALL never be rows; each row SHALL carry `contract`, `rank`, `repo`, `id`, `label`, `kind`, `path` relative to its repository and `line`. A repository that fails SHALL be asked once and listed in `unreachable`; one that answered from a building graph SHALL be listed in `building`. The section SHALL be bounded by a quarter of the budget, trimmed lowest rank first, SHALL NOT be shed to fit impacts, SHALL count trimmed rows in `omitted.cross_service`, and SHALL NOT cause a rejection: when mandatory evidence needs the room it SHALL shrink to a stub (`stub: true`, empty lists, with `contracts_found` and `rows_found`) and then be removed. A row read as a sentence SHALL say an endpoint is served and called, any other contract provided and used.

#### Scenario: An edit inside a handler names the other service's caller
- **GIVEN** a workspace of `api` and `web`, where `web`'s `loadStats` fetches `GET /api/v1/stats`
- **WHEN** change context runs on an edit inside the `api` handler for that route
- **THEN** `cross_service.contracts` is exactly `endpoint:GET /api/v1/stats`, and a row names `web`, `src/stats.ts`, `loadStats`, `relation: consumer`

#### Scenario: Endpoints merely reachable through the router are not touched
- **GIVEN** an edit inside one handler of a router file whose importers mount other routes
- **THEN** only the edited handler's endpoint is a served contract

#### Scenario: Moving a mount prefix names the old routes' callers
- **GIVEN** an app that mounts a router under `/api/v1`, and `web` calling `/api/v1/stats`
- **WHEN** change context runs on a diff moving the prefix to `/api/v2`
- **THEN** `endpoint:GET /api/v1/stats` is a removed contract and web's caller is a row

#### Scenario: A caller's edit names the provider
- **WHEN** change context runs on an edit inside web's `loadStats`
- **THEN** a `provider` row names api's handler file

#### Scenario: The section never causes a rejection
- **GIVEN** the smallest budget at which change context without a workspace succeeds
- **THEN** change context inside the workspace at that budget succeeds too

#### Scenario: Header, table and env contracts are touched
- **GIVEN** `readTenant` providing `header:X-Tenant-Id`, `sendTenant` using `header:x-org-id` and `env:ML_URL`, and `db/orders.ts` whose `createOrders` provides `table:orders`
- **THEN** a change to `readTenant` serves `header:x-tenant-id` at rank 1, a change to `sendTenant` uses `header:x-org-id` at rank 1 and `env:ML_URL`, and a change seeded at the file `db/orders.ts` serves `table:local:orders` at rank 1

#### Scenario: A table is asked under every spelling inside its database
- **GIVEN** home api and ml declared in database `turing`, web and billing not, `env: [{"name": "ML_URL", "service": "ml"}]`, and api touching `table:local:users`, `table:turing:audit`, `header:x-tenant-id` and `env:ML_URL`
- **THEN** ml answers `table:local:users` and `table:turing:audit` under its own `table:local:` ids, web (holding `table:turing:users`) answers under that named id, billing is never asked under its own `table:local:` id, web's sender of `header:x-tenant-id` is a row while the header node itself is not, and `env:ML_URL` has a provider row for ml of kind `service`; with billing as home, `table:local:users` is listed `local` and nobody is asked

#### Scenario: Repo-local contracts do not crowd out an endpoint
- **GIVEN** 25 touched `table:local:` contracts at rank 0 in a repo with no database, and an endpoint at rank 1
- **WHEN** at most 24 contracts may be asked
- **THEN** the endpoint is asked, its consumer is a row, and no contract is omitted
