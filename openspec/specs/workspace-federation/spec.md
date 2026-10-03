# workspace-federation Specification

## Purpose
TBD - created by archiving change federate-workspace-queries. Update Purpose after archive.
## Requirements
### Requirement: A workspace is a manifest naming member repositories
A directory SHALL be a workspace exactly when it holds `cgraph.workspace.json`, a JSON object with an optional `name` (defaulting to the directory name) and a non-empty `repos` array of objects with a non-empty `name` and `root`, each root resolved against the workspace directory when relative. A missing file, malformed JSON, a non-object manifest, an empty or absent `repos` array, a duplicate repo name, or a root that is not an existing directory SHALL make the workspace invalid: it SHALL carry an error, SHALL expose no repos, and SHALL refuse every operation with `ok:false` and `code: "workspace_invalid"`. `cgraph workspace init` SHALL write the manifest, storing a repo root relative when it lies under the workspace, and SHALL discover every immediate subdirectory holding a `.git` entry, sorted by name, when no repo is named explicitly. `cgraph workspace status` SHALL print each repo's daemon state with node and edge counts, and the workspace totals.

#### Scenario: A valid manifest loads and round-trips
- **GIVEN** `cgraph.workspace.json` naming `api` at `./api` and `web` at `./web`, both existing
- **THEN** the directory is a workspace, its repos load in manifest order with absolute roots, and re-serializing stores the roots as `./api` and `./web`

#### Scenario: Every invalid manifest is loud
- **GIVEN** a missing file, `{not json`, `{"repos": []}`, two repos both named `a`, or a repo whose root does not exist
- **THEN** each load reports an error with no repos, and an operation against it returns `code: "workspace_invalid"`

#### Scenario: Discovery finds the repositories one level down
- **GIVEN** a directory containing `api/.git`, `web/.git`, `.hidden/.git` and `notarepo`
- **THEN** discovery returns `api` and `web`, in that order

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

### Requirement: Merged reads are tagged, and a repository that cannot answer fully is reported
A federated `status` SHALL report each repository and totals counting only the reachable ones; `query` SHALL sum totals, merge hits ranked by centrality then label, truncate to the caller's limit and tag each hit with its `repo`; `explain` SHALL answer from the first repository that has the node, tagged with its `repo` and, when other repositories have the same node, `also_in` naming them, and SHALL otherwise return `found:false` with `repos_searched`; `update` SHALL reach every repository and report each. Every federated result SHALL carry `unreachable` listing each repository whose daemon could not be reached or that answered with an error, with its name, root and the error, and SHALL carry `building` naming each repository that answered from a graph it is still building, together with a `note` saying the answer is short until it finishes. Operations whose unit is one project (`report`, `context`, `remember`, `recall`, `shutdown`) SHALL be refused with `ok:false`, `code: "workspace_op_unsupported"` and a message naming each repository root to use instead.

#### Scenario: A down repository does not make the answer look total
- **GIVEN** a workspace of two repositories where one daemon cannot be reached
- **WHEN** `status` runs
- **THEN** the answer succeeds, `totals.reachable` is 1, the down repository's entry carries `reachable:false`, and `unreachable` names it with its error

#### Scenario: A repository still building is named
- **GIVEN** a workspace of two repositories where one answers with `graph_state: "building"`
- **THEN** the result carries `building` naming that repository and a note that its witnesses are missing until the build finishes

#### Scenario: Merged hits are ranked and tagged
- **GIVEN** `api` returning two hits of centrality 0.4 and 0.1 and `web` one of 0.9, and a limit of 2
- **THEN** `total` is 3, the first hit is the `web` one, each hit carries its `repo`, and the result is marked truncated

#### Scenario: A per-project op names the repositories
- **WHEN** `report` is requested at a workspace root
- **THEN** the answer is `ok:false` with `code: "workspace_op_unsupported"` and the message contains each repository's name and root

### Requirement: A repository inside a workspace finds it and crosses it for impact and path
A project root SHALL belong to the workspace whose manifest sits in the nearest ancestor directory of the path as given or as resolved, up to and including `$HOME`, and lists a repository whose root is the project root or contains it; the most specific such repository is `home`, and the home repository SHALL be answered from the project root. From such a root the thin client SHALL federate `impact` and `path` across the workspace, tagging the result with `workspace: {name, home}`, and SHALL answer every other op from the home repository alone. Asks forwarded to member repositories SHALL NOT federate again. An `expected_content_root` pin SHALL be sent to the home repository only, and a home pin that fails SHALL fail the request. A manifest that lists the root but cannot be loaded SHALL be reported in `workspace.errors`, with the answer taken from the home repository alone.

#### Scenario: Impact from a member root reaches the other service
- **GIVEN** a workspace of `api`, which serves `GET /api/v1/stats`, and `web`, whose `loadStats` fetches it
- **WHEN** `impact` runs on `endpoint:GET /api/v1/stats` with the `api` repository as the root
- **THEN** the answer contains `loadStats` tagged `repo: web`, and `workspace.home` is `api`

#### Scenario: Query from a member root stays home
- **WHEN** `query` for `loadStats` runs with the `api` repository as the root
- **THEN** no node from `web` is returned

#### Scenario: A nested worktree is answered from itself
- **GIVEN** a worktree at `api/.agents/worktrees/feature`
- **THEN** its enclosing workspace names `api` as home, with `api`'s root replaced by the worktree

#### Scenario: Unlisted directories and manifests above HOME are not used
- **GIVEN** a directory beside the members that the manifest does not list, or `$HOME` set below the workspace directory, including `$HOME` spelled through a symlink
- **THEN** no enclosing workspace is found

#### Scenario: A pin names the home graph
- **WHEN** a member-root `impact` carries a wrong `expected_content_root`
- **THEN** it fails; with the home repository's own content root it succeeds and still returns the other service's caller

#### Scenario: A manifest naming a member not present here is reported
- **GIVEN** a manifest listing `api` and `billing`, where `billing` does not exist
- **THEN** the root `api` still finds the manifest, and the errors are returned rather than the workspace being skipped

### Requirement: A workspace manifest declares proxy prefixes between members
`cgraph.workspace.json` SHALL accept an optional `prefixes` array of `{"repo", "from", "to"}` objects with absolute paths, normalized (duplicate and trailing slashes dropped). An entry that is malformed, maps a path to itself, or names a repository the manifest does not list SHALL make the manifest unusable, reported in `errors`. Federated `impact` SHALL cross from a provider's endpoint to a member's spelling of it under that member's `from`, and from such a spelling to the provider, only when the member consumes that spelling without serving it; a contract reached only through a member's proxy SHALL NOT be asked back of that member. A member's proxied spelling SHALL NOT be crossed at an endpoint only that member serves, nor at a contract the traversal reached only inside that member. A `prefixes` entry, `repos` entry or manifest `name` whose members are not strings SHALL be a manifest error, never an exception. Federated `path` SHALL join across the proxy and keep both spellings on the path, with `bridged_through` naming the provider's id.

#### Scenario: Impact crosses the proxy both ways
- **GIVEN** a workspace of `idp` and `web` with `{"repo": "web", "from": "/api/backend", "to": "/api"}`, where web's hook consumes `endpoint:PATCH /api/backend/v1/sessions/{}/extend`
- **WHEN** `impact` runs on idp's handler with `dependents`
- **THEN** the hook is reached, tagged `repo: web`, `bridged_through: endpoint:PATCH /api/v1/sessions/{}/extend`; and `impact` from the hook with `dependencies` reaches idp's handler

#### Scenario: A route the member serves itself is not crossed
- **GIVEN** web serves `GET /api/backend/healthz` itself
- **THEN** impact from idp's `GET /api/healthz` handler does not reach web's healthz route

#### Scenario: A prefix naming no member is an error
- **GIVEN** a `prefixes` entry for a repository the manifest does not list, or a relative `from`
- **THEN** the workspace carries errors and no repos

#### Scenario: A proxied call does not join a third member at the caller's own route
- **GIVEN** members idp, web and mobile, prefix `web:/api/backend=/api`, web alone serving `GET /api/saml/metadata`, web calling `/api/backend/saml/metadata`, and mobile calling `GET /api/saml/metadata`
- **THEN** `impact` from web's caller (either direction) and from `endpoint:GET /api/backend/saml/metadata` does not reach mobile, and `path` from web's caller to mobile's is empty; once idp serves the path too, all three cross

#### Scenario: A member's own route does not reach its proxied callers
- **GIVEN** web serves `GET /api/saml/metadata` and its `getIdpMetadata` calls `/api/backend/saml/metadata` through the prefix
- **WHEN** `impact` runs on web's route handler with `dependents`
- **THEN** `getIdpMetadata` is not reached, whether or not idp also serves the path; when idp serves it, `impact` on idp's handler reaches `getIdpMetadata`

#### Scenario: A non-string manifest member is an error
- **GIVEN** a `prefixes` entry `{"repo": 7}`, a `repos` entry `{"name": 5}`, or `"name": 3`
- **THEN** `load_workspace` returns errors and no repos, and does not throw

### Requirement: A workspace manifest declares shared databases and env providers
`cgraph.workspace.json` SHALL accept an optional `databases` array of `{"name", "repos": [...]}` and an optional `env` array of `{"name", "service"}`, round-tripped by `workspace_manifest_json`. A database name SHALL have no `:` or whitespace and SHALL NOT be `local`; its `repos` SHALL be a non-empty list of member names. An entry that is malformed or non-string, a repo or service the manifest does not list, a repo in two databases, or a database or env name declared twice SHALL make the manifest unusable, reported in `errors`, never an exception.

#### Scenario: Declarations load and round-trip
- **GIVEN** `"databases": [{"name": "turing", "repos": ["api", "ml"]}]` and `"env": [{"name": "ML_BACKEND_URL", "service": "ml"}]`
- **THEN** the workspace loads and its manifest JSON carries both unchanged

#### Scenario: A bad declaration is an error
- **GIVEN** a database naming a repo the manifest does not list, a repo in two databases, a name declared twice, the name `local`, a name with `:`, a non-string member, or an env naming an unknown service
- **THEN** `load_workspace` returns errors and no repos, and does not throw

### Requirement: A workspace manifest declares claim issuers
`cgraph.workspace.json` SHALL accept an optional `issuers` array of `{"name", "repos": [...]}`, round-tripped by `workspace_manifest_json` and validated as `databases` are: a name with `:` or whitespace, the name `local`, an empty, non-array or non-string `repos`, a repo the manifest does not list, a repo in two issuers, or an issuer declared twice SHALL make the manifest unusable, reported in `errors`, never an exception. `impact` and `path` SHALL cross at a member's claim only towards the other members of its issuer, at `crossing_id`'s `claim:<issuer>:<name>`, asking each member under its own `claim:<name>`; a registered RFC 7519 claim SHALL not cross, and a repo outside the issuer SHALL never be asked about it.

#### Scenario: Impact crosses within the issuer only
- **GIVEN** repos idp, web and api, `"issuers": [{"name": "idp", "repos": ["idp", "web"]}]`, idp's `mintToken` reaching `claim:session_id`, `claim:email` and `claim:sub`, and web and api each holding a reader of all three
- **THEN** impact from `mintToken` reaches exactly web's `session_id` reader through `claim:idp:session_id` and web's `email` reader through `claim:idp:email`
- **AND** with no `issuers`, it reaches exactly web's and api's `session_id` readers through `claim:session_id`

#### Scenario: A bad issuer is an error
- **GIVEN** an issuer naming a repo the manifest does not list, a repo in two issuers, a name declared twice, the name `local`, a name with `:`, a non-string repo, an empty `repos`, or `issuers` that is not an array
- **THEN** `load_workspace` returns errors and no repos, and does not throw

