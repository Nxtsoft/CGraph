# workspace-federation Specification

## Purpose
One read surface over several repositories, each still served by its own resident daemon, joined at the contract nodes they share. No graph is copied into a workspace; every repository keeps its own watcher and incremental updates, so an edit on either side is live on the next query.

## ADDED Requirements

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
A federated `impact` SHALL ask every member repository about the seed and treat those that have it as the owners, naming them in `repos`. Every `endpoint:` node reached (and the seed itself when it is one) SHALL be forwarded once to every member repository with `max_depth` reduced by the depth at which the contract was reached, and the returned witnesses SHALL be merged at that depth plus their own, each tagged with its `repo` and, when it came from across a contract, `bridged_through` naming it. The seed SHALL NOT be a witness of itself in any repository; witnesses SHALL be ordered by depth, then centrality, then repo, then label, and truncated to the caller's limit with `total` counting all of them; the contracts crossed SHALL be listed in `bridged`. A seed no repository has SHALL answer `found:false` with no witnesses rather than an empty success.

#### Scenario: A handler's blast radius reaches the other repository's consumer
- **GIVEN** repo `api` where the handler's dependents at depth 1 are its file and `endpoint:GET /api/v1/notebooks/starred-notes`, and repo `web` which does not have the handler but whose dependents of that endpoint are `notebooksApi` at 1 and `useStarred` at 2
- **WHEN** `impact` runs at the workspace root with `max_depth` 3
- **THEN** `repos` is `["api"]`, the endpoint is a witness at depth 1 in `api`, `notebooksApi` is one at depth 2 in `web` and `useStarred` at depth 3 in `web`, both carrying `bridged_through` the endpoint, and `bridged` names that endpoint at depth 1

#### Scenario: The depth budget bounds the crossing
- **GIVEN** the same graphs and `max_depth` 1
- **THEN** the endpoint is reached and no witness from the other repository is returned

### Requirement: A path joins two repositories at a contract
A federated `path` SHALL return the answer of any member repository that has both ends. Otherwise it SHALL take the endpoints the source reaches in its repository, nearest first and at most eight, and for each try a path from the source to that contract and from that contract to the target in another repository, returning the first pair concatenated at the contract (named once), with `bridged_through` the contract, `repos` the two repositories, and each path node tagged with the repository it is in. When no contract bridges the two, the result SHALL be an empty path rather than a fabricated one.

#### Scenario: A handler reaches a hook in the other repository
- **GIVEN** a source in `api` whose path to `endpoint:GET /api/v1/notebooks` exists there, and a target in `web` whose path from that endpoint exists there
- **THEN** the returned path is source, endpoint, target, `bridged_through` is the endpoint, `repos` is `["api", "web"]`, and the first and last nodes carry `api` and `web`

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
