## ADDED Requirements

### Requirement: graph_report exposes module grouping
The MCP `graph_report` tool SHALL describe a `group_by` parameter for the `modules` view, taking `auto`, `packages` or `depth`, and SHALL forward it verbatim to the daemon `report` op. The bundled `cgraph` host skill and `docs/host-skill-contract.md` SHALL say that a monorepo's module map is named by its workspace packages by default, and that the response's `group_by` and `manifest` say which question the diagram answered.

#### Scenario: group_by is forwarded
- **WHEN** a client calls `graph_report` with `view: "modules"` and `group_by: "depth"`
- **THEN** the daemon `report` request carries `group_by` `depth` and the response echoes `group_by` `depth`
