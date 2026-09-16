## ADDED Requirements

### Requirement: graph_report exposes the design view

The MCP `graph_report` tool SHALL describe `view: "design"` as the entry-point and call-flow report, SHALL accept `hops` (integer) and forward it verbatim to the daemon `report` op, and SHALL no longer describe any view as reserved. The bundled `cgraph` host skill and `docs/host-skill-contract.md` SHALL route questions about how a program starts, its main flows, where a request goes, or a program-design overview to `graph_report` with `view: "design"`.

#### Scenario: Design parameters are forwarded
- **WHEN** a client calls `graph_report` with `view: "design"`, `format: "mermaid"` and `hops: 2`
- **THEN** the daemon `report` request carries those values and the response echoes `hops` 2 with a `flowchart TD` in `rendered`
