## ADDED Requirements

### Requirement: Change context names the other services a change crosses into
When the target root belongs to a workspace, change context SHALL add a `cross_service` section naming the workspace and home repository, the contracts the change touches, and rows for the other repositories. A change SHALL serve an endpoint it edits, one whose handler it reaches through `handled_by`, or one a changed file directly contains; it SHALL call an endpoint that changed code consumes, or that a function calling a changed helper consumes. For each served endpoint the other repositories' direct `CONSUMES` callers SHALL be rows with `relation: consumer`; for each called endpoint their `handled_by` handler SHALL be a row with `relation: provider`; each row SHALL carry `contract`, `repo`, `id`, `label`, `kind`, `path` relative to its repository, `line` and `depth`. At most 24 contracts SHALL be asked. The section SHALL be bounded by a quarter of the budget (at least 256 tokens), SHALL NOT be shed to fit impacts, SHALL count trimmed rows in `omitted.cross_service`, and SHALL list each repository that could not answer in `unreachable` and each that answered from a building graph in `building`.

#### Scenario: An edit inside a handler names the other service's caller
- **GIVEN** a workspace of `api` and `web`, where `web`'s `loadStats` fetches `GET /api/v1/stats`
- **WHEN** change context runs on an edit inside the `api` handler for that route
- **THEN** `cross_service.contracts` is exactly `endpoint:GET /api/v1/stats`, and a row names `web`, `src/stats.ts`, `loadStats`, `relation: consumer`

#### Scenario: Endpoints merely reachable through the router are not touched
- **GIVEN** an edit inside one handler of a router file whose importers mount other routes
- **THEN** only the edited handler's endpoint is a served contract
