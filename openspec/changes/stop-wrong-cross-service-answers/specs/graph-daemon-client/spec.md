## ADDED Requirements

### Requirement: Graph reads wait out a building daemon
The thin client SHALL re-ask a daemon whose answer carries `graph_state: "building"` for the graph-reading operations `query`, `path`, `explain`, `impact`, `context`, `report` and `recall`, backing off between asks (at most 250 ms apart), until the answer no longer says `building` or `ClientRequest::build_wait` (default 30 seconds) runs out, and SHALL then return the last answer unchanged, so a timed-out answer still carries its `building` marker. `status`, `update`, `shutdown` and `remember` SHALL answer from the first reply without waiting, and a `build_wait` of zero SHALL disable the wait. A workspace request SHALL share one `build_wait` across every member ask and contract hop, each ask getting only the time left, so the whole federated request is bounded by one wait; a member whose build finishes within it SHALL contribute its witnesses.

#### Scenario: A cold workspace member is waited for
- **GIVEN** a workspace of `api`, which serves `GET /api/v1/stats`, and `web`, whose `loadStats` fetches it, with `web`'s build held until 400 ms after the request
- **WHEN** `impact` runs on `endpoint:GET /api/v1/stats` at the workspace root
- **THEN** the answer contains a node tagged `repo: "web"` and carries no `building` list

#### Scenario: A zero wait returns the building answer
- **GIVEN** the same workspace with `build_wait` set to zero
- **WHEN** the same `impact` runs
- **THEN** the answer carries `building` naming `web` and contains no `web` node

#### Scenario: A wait that runs out bounds the whole workspace request
- **GIVEN** the same workspace with `web`'s build never released and `build_wait` of 1.5 s
- **WHEN** the same `impact` runs, which asks both members and then each member again for the contract it reached
- **THEN** it returns within about one wait (under 2.4 s), carrying `building` naming `web`

## MODIFIED Requirements

### Requirement: Thin client command surface
The system SHALL provide thin client commands for `query`, `path`, `explain`, `update`, `status`, and `shutdown` that do not rebuild the graph for each request in daemon mode.

#### Scenario: Query uses resident graph
- **WHEN** a user runs a thin client `query` command while the daemon has a loaded graph
- **THEN** the client sends one request and prints the daemon response without running the full pipeline locally

#### Scenario: Query waits for a building graph
- **WHEN** a user runs a thin client `query` command while the daemon is still building its first graph
- **THEN** the client re-asks until the build publishes or the build wait runs out, and prints the last response
