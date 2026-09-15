## ADDED Requirements

### Requirement: MCP exposes graph_report
The MCP server SHALL list a `graph_report` tool whose `view` parameter is the enum `modules` | `design` | `clones` | `types`, with `format` (`json` | `mermaid` | `markdown` | `svg`), `scope`, `depth`, `include_tests`, `budget` and `expected_content_root`. Arguments SHALL be forwarded verbatim to the daemon `report` op. When the daemon answers `unknown op: report`, the tool error SHALL be the upgrade hint. The bundled `cgraph` host skill and `docs/host-skill-contract.md` SHALL route architecture and module-map questions to `graph_report`.

#### Scenario: Tool is advertised and forwarded
- **WHEN** a client calls `tools/list`
- **THEN** `graph_report` appears with `view` as an enum of the four views
- **AND** `tools/call graph_report {view: "modules", format: "mermaid"}` forwards `{op: "report", params: {view, format}}` to the daemon
