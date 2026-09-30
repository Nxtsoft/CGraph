## ADDED Requirements

### Requirement: A repository inside a workspace finds it and crosses it for impact and path
A project root SHALL belong to the workspace whose manifest sits in the nearest ancestor directory, up to and including `$HOME`, and lists a repository whose root is the project root or contains it; that repository is `home`, and the home repository SHALL be answered from the project root. From such a root the thin client SHALL federate `impact` and `path` across the workspace, tagging the result with `workspace: {name, home}`, and SHALL answer every other op from the home repository alone. Asks forwarded to member repositories SHALL NOT federate again.

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
- **GIVEN** a directory beside the members that the manifest does not list, or `$HOME` set below the workspace directory
- **THEN** no enclosing workspace is found
