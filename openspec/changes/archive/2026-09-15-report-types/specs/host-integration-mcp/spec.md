## ADDED Requirements

### Requirement: graph_report exposes the types view

The MCP `graph_report` tool SHALL describe `view: "types"` as the type-definition audit (identical, duplicates, overlaps, unreferenced), SHALL accept `threshold` (number) and `min_members` (integer), and SHALL forward them verbatim to the daemon `report` op. The bundled `cgraph` host skill and `docs/host-skill-contract.md` SHALL route questions about type bloat, duplicate or redundant interfaces and structs, and dead types to `graph_report` with `view: "types"`, and SHALL say that `unreferenced` reflects cross-file references only.

#### Scenario: Types parameters are forwarded
- **WHEN** a client calls `graph_report` with `view: "types"`, `threshold: 0.6` and `min_members: 1`
- **THEN** the daemon `report` request carries those three values and the response echoes `threshold` 0.6 and `min_members` 1
