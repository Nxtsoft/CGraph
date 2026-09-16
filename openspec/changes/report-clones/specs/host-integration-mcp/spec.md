## ADDED Requirements

### Requirement: graph_report exposes the clones view

The MCP `graph_report` tool SHALL describe `view: "clones"` as the duplicate-code report, SHALL accept `min_tokens` (integer) alongside `threshold`, and SHALL forward them verbatim to the daemon `report` op. The bundled `cgraph` host skill and `docs/host-skill-contract.md` SHALL route questions about copy-pasted code, near-duplicate functions and what to extract into a shared helper to `graph_report` with `view: "clones"`, and SHALL say that a `hint` in the response means a rescan is needed.

#### Scenario: Clones parameters are forwarded
- **WHEN** a client calls `graph_report` with `view: "clones"`, `threshold: 0.6` and `min_tokens: 10`
- **THEN** the daemon `report` request carries those values and the response echoes `threshold` 0.6 and `min_tokens` 10
