## ADDED Requirements

### Requirement: A workspace manifest declares shared databases and env providers
`cgraph.workspace.json` SHALL accept an optional `databases` array of `{"name", "repos": [...]}` and an optional `env` array of `{"name", "service"}`, round-tripped by `workspace_manifest_json`. A database name SHALL have no `:` or whitespace and SHALL NOT be `local`; its `repos` SHALL be a non-empty list of member names. An entry that is malformed or non-string, a repo or service the manifest does not list, a repo in two databases, or a database or env name declared twice SHALL make the manifest unusable, reported in `errors`, never an exception.

#### Scenario: Declarations load and round-trip
- **GIVEN** `"databases": [{"name": "turing", "repos": ["api", "ml"]}]` and `"env": [{"name": "ML_BACKEND_URL", "service": "ml"}]`
- **THEN** the workspace loads and its manifest JSON carries both unchanged

#### Scenario: A bad declaration is an error
- **GIVEN** a database naming a repo the manifest does not list, a repo in two databases, a name declared twice, the name `local`, a name with `:`, a non-string member, or an env naming an unknown service
- **THEN** `load_workspace` returns errors and no repos, and does not throw

## MODIFIED Requirements

### Requirement: Impact crosses a contract once, with the depth that remains
A federated `impact` SHALL ask every member repository about the seed and treat those that have it as the owners, naming them in `repos`. Every contract node reached that crosses repositories (and the seed itself when it is one) SHALL be forwarded once to every member repository with `max_depth` reduced by the depth at which the contract was reached, and the returned witnesses SHALL be merged at that depth plus their own, each tagged with its `repo` and, when it came from across a contract, `bridged_through` naming it. A contract crosses repositories as `crossing_id` says: every `endpoint:` and `claim:` id, a `header:` id whose name is not a standard HTTP header (the IANA permanent field names and the common tracing and proxy headers), a `table:` or `label:` id in a named database, a member's `table:local:<name>` or `label:local:<name>` at `table:<database>:<name>` when the manifest declares it in that database (asked of the other members of that database under their own local spelling, and of no other repository), and an `env:` id only when the manifest's `env` declares it; an undeclared env id, a standard header and an undeclared repo-local table SHALL NOT cross. The seed SHALL NOT be a witness of itself in any repository; witnesses SHALL be ordered by depth, then centrality, then repo, then label, and truncated to the caller's limit with `total` counting all of them; the contracts crossed SHALL be listed in `bridged`, each with `contract` and its depth, an endpoint also under `endpoint`, and a declared env id with `provided_by` naming its service. A seed no repository has SHALL answer `found:false` with no witnesses rather than an empty success.

#### Scenario: A handler's blast radius reaches the other repository's consumer
- **GIVEN** repo `api` where the handler's dependents at depth 1 are its file and `endpoint:GET /api/v1/notebooks/starred-notes`, and repo `web` which does not have the handler but whose dependents of that endpoint are `notebooksApi` at 1 and `useStarred` at 2
- **WHEN** `impact` runs at the workspace root with `max_depth` 3
- **THEN** `repos` is `["api"]`, the endpoint is a witness at depth 1 in `api`, `notebooksApi` is one at depth 2 in `web` and `useStarred` at depth 3 in `web`, both carrying `bridged_through` the endpoint, and `bridged` names that endpoint at depth 1

#### Scenario: The depth budget bounds the crossing
- **GIVEN** the same graphs and `max_depth` 1
- **THEN** the endpoint is reached and no witness from the other repository is returned

#### Scenario: Impact crosses at an application header, never at a standard one or an undeclared env name
- **GIVEN** api's `readTenant` reaching `header:x-tenant-id`, `header:authorization` and `env:NODE_ENV`, and web holding all three
- **THEN** `impact` from `readTenant` reaches web's `sendTenant` (depth 2, `bridged_through: header:x-tenant-id`, `bridged` entry with `contract` and no `endpoint` key) and never web's `fetchWithToken` or `isProduction`; `impact` from `sendTenant` with `dependencies` reaches `readTenant`

#### Scenario: A table crosses inside its declared database only
- **GIVEN** members api, ml and billing, `databases: [{"name": "turing", "repos": ["api", "ml"]}]`, and each holding `table:local:users`
- **THEN** `impact` from api's `createUsers` reaches ml's `listUsers` with `bridged_through: table:turing:users` and never billing's `chargeUsers`; without the declaration nothing outside api is reached

#### Scenario: A declared env variable names its provider
- **GIVEN** `env: [{"name": "API_URL", "service": "api"}]` and ml's `callApi` reaching `env:API_URL`
- **THEN** the `bridged` list has `{"contract": "env:API_URL", "provided_by": "api"}`

### Requirement: A path joins two repositories at a contract
A federated `path` SHALL return the answer of any member repository that has both ends. Otherwise it SHALL take the contracts the source reaches in its repository that cross repositories (as for `impact`), nearest first and at most eight, and for each try a path from the source to that contract and from that contract to the target in another repository, returning the first pair concatenated at the contract (named once), with `bridged_through` the contract, `repos` the two repositories, and each path node tagged with the repository it is in. When no contract bridges the two, the result SHALL be an empty path rather than a fabricated one; two repositories that share only a standard header or an undeclared env name SHALL get an empty path.

#### Scenario: A handler reaches a hook in the other repository
- **GIVEN** a source in `api` whose path to `endpoint:GET /api/v1/notebooks` exists there, and a target in `web` whose path from that endpoint exists there
- **THEN** the returned path is source, endpoint, target, `bridged_through` is the endpoint, `repos` is `["api", "web"]`, and the first and last nodes carry `api` and `web`

#### Scenario: A path joins two repositories at a header
- **GIVEN** web's `sendTenant` with a path to `header:x-tenant-id` and api's path from it to `readTenant`
- **THEN** the path is `[sendTenant, header:x-tenant-id, readTenant]` with `bridged_through: header:x-tenant-id`

#### Scenario: NODE_ENV and Authorization join nothing
- **GIVEN** web's `isProduction` reaching `env:NODE_ENV` and `header:authorization`, both of which have a path to api's `readTenant` in api, and no declaration
- **THEN** the path from `isProduction` to `readTenant` is empty
