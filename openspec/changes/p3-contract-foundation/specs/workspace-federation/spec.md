## ADDED Requirements

### Requirement: A workspace manifest declares shared databases and env providers
`cgraph.workspace.json` SHALL accept an optional `databases` array of `{"name", "repos": [...]}` and an optional `env` array of `{"name", "service"}`, round-tripped by `workspace_manifest_json`. A database name SHALL have no `:` or whitespace and SHALL NOT be `local`; its `repos` SHALL be a non-empty list of member names. An entry that is malformed or non-string, a repo or service the manifest does not list, a repo in two databases, or a database or env name declared twice SHALL make the manifest unusable, reported in `errors`, never an exception.

#### Scenario: Declarations load and round-trip
- **GIVEN** `"databases": [{"name": "turing", "repos": ["api", "ml"]}]` and `"env": [{"name": "ML_BACKEND_URL", "service": "ml"}]`
- **THEN** the workspace loads and its manifest JSON carries both unchanged

#### Scenario: A bad declaration is an error
- **GIVEN** a database naming a repo the manifest does not list, a repo in two databases, a name declared twice, the name `local`, a name with `:`, a non-string member, or an env naming an unknown service
- **THEN** `load_workspace` returns errors and no repos, and does not throw

### Requirement: Impact and path cross at every bridged contract and inside a declared database
Federated `impact` and `path` SHALL cross at every contract `is_bridged_contract` accepts, exactly as at endpoints. A member's `table:local:<name>` or `label:local:<name>` SHALL cross at `table:<database>:<name>` when the member is declared in that database, reaching the other members of it under their own repo-local spelling and no other repository; with no declaration it SHALL NOT cross. Each `bridged` entry SHALL carry the crossed id under `contract`, and endpoints also under `endpoint` as before. A reached `env:<NAME>` with a declared provider SHALL be listed in `bridged` with `provided_by`.

#### Scenario: Impact and path cross at a header
- **GIVEN** api's `readTenant` reaching `header:x-tenant-id` and web's `sendTenant` consuming it
- **THEN** `impact` from `readTenant` reaches `sendTenant` (repo web, depth 2, `bridged_through: header:x-tenant-id`), `impact` from `sendTenant` with `dependencies` reaches `readTenant`, and `path` from `sendTenant` to `readTenant` is `[sendTenant, header:x-tenant-id, readTenant]`

#### Scenario: A table crosses inside its declared database only
- **GIVEN** members api, ml and billing, `databases: [{"name": "turing", "repos": ["api", "ml"]}]`, and each holding `table:local:users`
- **THEN** `impact` from api's `createUsers` reaches ml's `listUsers` with `bridged_through: table:turing:users` and never billing's `chargeUsers`; without the declaration nothing outside api is reached

#### Scenario: An env variable names its provider
- **GIVEN** `env: [{"name": "API_URL", "service": "api"}]` and ml's `callApi` reaching `env:API_URL`
- **THEN** the `bridged` list has `{"contract": "env:API_URL", "provided_by": "api"}`
