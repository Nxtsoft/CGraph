## ADDED Requirements

### Requirement: MCP server lists a host-chosen tool subset
`cgraph-mcp` SHALL accept `--tools` with a comma-separated list of tool names and SHALL advertise
only those tools in `tools/list`, in the server's own order. `all`, or no `--tools`, SHALL list
every tool. A name the server does not implement SHALL make the server exit with status 2 before
serving. The Claude Code plugin SHALL list `graph_query`, `graph_context` and `graph_impact`
unless `CGRAPH_MCP_TOOLS` names others, and its `cgraph-enrich` skill SHALL NOT be
model-invocable.

#### Scenario: Plugin lists the core tools
- **WHEN** Claude Code starts with the plugin installed and `CGRAPH_MCP_TOOLS` unset
- **THEN** the session lists exactly the three core cgraph tools, and its first request's prefix
  is at most 150 tokens larger than the same session without the plugin

#### Scenario: Unknown tool name fails loud
- **WHEN** `cgraph-mcp --tools graph_nope` starts
- **THEN** it prints the unknown name and exits with status 2

### Requirement: MCP server prewarms an indexed project
When `cgraph-mcp` starts and `<project root>/cgraph-out/` exists, it SHALL start the project's
daemon in the background without waiting for it. When that directory does not exist it SHALL NOT
start a daemon, so an unindexed repository is not written to until a tool is called.

#### Scenario: Indexed project
- **WHEN** `cgraph-mcp` starts for a root with `cgraph-out/` and receives only `initialize`
- **THEN** a daemon for that root is running within seconds

#### Scenario: Unindexed project
- **WHEN** `cgraph-mcp` starts for a root without `cgraph-out/`
- **THEN** no daemon starts and the root gains no `cgraph-out/`
