## ADDED Requirements

### Requirement: Status reports unresolved routes and calls
Daemon `status` SHALL carry `route_resolution` with the fields `stats.json` uses (`routes`, `routes_unresolved`, `mounts`, `mounts_unresolved`, `endpoints`, `calls`, `calls_unresolved`, `consumes`, `endpoints_external`, `endpoints_documented`), from the most recent full rescan or incremental update, saved with the persisted graph and restored when a daemon fast-loads it; it SHALL carry `null` only when no such tally exists. A workspace `status` SHALL carry it for each reachable repository.

#### Scenario: A consumer repo's status counts its calls
- **GIVEN** a repository whose three functions each `fetch` a literal API path
- **WHEN** `status` runs after the daemon's first build
- **THEN** `route_resolution.calls` is at least 3 and `calls_unresolved` is 0

### Requirement: The thin client names the services behind a file's endpoints
`cgraph-client cross-service` SHALL take a file inside a workspace member (a relative path being relative to the project root) and return, for each endpoint the file declares (`contains` from the file) or calls (`CONSUMES` from its functions or class methods, never from files it imports), the other repositories' callers or handlers, as the `cross_service` section and plain `summary` lines, naming the home repository when it is still building and the endpoints a contract cap left unchecked; outside a workspace it SHALL return `workspace: null` and no lines. `impact` SHALL accept `relation` as a list of relation names to follow.

#### Scenario: A routes file names its callers
- **WHEN** the lookup runs on a file declaring `GET /api/v1/stats`, which another repository's `loadStats` fetches
- **THEN** a summary line reads `… serves GET /api/v1/stats, called from web src/stats.ts:… (loadStats)`

#### Scenario: An importer claims nothing
- **WHEN** the lookup runs on a file that only imports a routes file
- **THEN** no summary lines are returned
