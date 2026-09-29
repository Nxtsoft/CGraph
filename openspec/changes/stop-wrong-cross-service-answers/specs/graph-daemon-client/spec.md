## ADDED Requirements

### Requirement: Graph reads wait out a building daemon
The thin client SHALL re-ask a daemon whose answer carries `graph_state: "building"` for the graph-reading operations `query`, `path`, `explain`, `impact`, `context`, `report` and `recall`, backing off between asks (at most 250 ms apart), until the answer no longer says `building` or `ClientRequest::build_wait` (default 30 seconds) runs out, and SHALL then return the last answer unchanged, so a timed-out answer still carries its `building` marker. `status`, `update`, `shutdown` and `remember` SHALL answer from the first reply without waiting, and a `build_wait` of zero SHALL disable the wait. Because every member of a workspace is asked through the same client path, a federated read SHALL include the witnesses of a member whose build finishes within the wait.

#### Scenario: A cold workspace member is waited for
- **GIVEN** a workspace of `api`, which serves `GET /api/v1/stats`, and `web`, whose `loadStats` fetches it, with `web`'s build held until 400 ms after the request
- **WHEN** `impact` runs on `endpoint:GET /api/v1/stats` at the workspace root
- **THEN** the answer contains a node tagged `repo: "web"` and carries no `building` list

#### Scenario: A zero wait returns the building answer
- **GIVEN** the same workspace with `build_wait` set to zero
- **WHEN** the same `impact` runs
- **THEN** the answer carries `building` naming `web` and contains no `web` node
