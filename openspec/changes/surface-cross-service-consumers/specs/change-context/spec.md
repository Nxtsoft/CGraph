## ADDED Requirements

### Requirement: Change context names the other services a change crosses into
When the target root belongs to a workspace, change context SHALL add a `cross_service` section naming the workspace and home repository, the contracts the change touches, and rows for the other repositories. A change SHALL serve an endpoint it edits, one whose handler it reaches through `handled_by`, or one a changed file directly contains; it SHALL remove an endpoint served in the base snapshot and not the target, and add one served in the target and not the base, marking such a contract `outside_diff` and ranking it after every other when the roots also differ in files the diff does not supply; it SHALL call an endpoint that changed code consumes, or that a function calling a changed helper consumes. Contracts SHALL be ranked (edited, removed or added first) and at most 24 asked, in rank order. For each served, removed or added endpoint the other repositories' direct `CONSUMES` callers SHALL be rows with `relation: consumer`; for each called endpoint their `handled_by` handler SHALL be a row with `relation: provider`; each row SHALL carry `contract`, `rank`, `repo`, `id`, `label`, `kind`, `path` relative to its repository and `line`. A repository that fails SHALL be asked once and listed in `unreachable`; one that answered from a building graph SHALL be listed in `building`. The section SHALL be bounded by a quarter of the budget, trimmed lowest rank first, SHALL NOT be shed to fit impacts, SHALL count trimmed rows in `omitted.cross_service`, and SHALL NOT cause a rejection: when mandatory evidence needs the room it SHALL shrink to a stub (`stub: true`, empty lists, with `contracts_found` and `rows_found`) and then be removed.

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
