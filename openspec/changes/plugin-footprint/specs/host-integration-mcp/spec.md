## ADDED Requirements

### Requirement: MCP server lists a host-chosen tool subset
`cgraph-mcp` SHALL accept `--tools` with a comma-separated list of tool names and SHALL advertise
only those tools in `tools/list`, in the server's own order, ignoring whitespace around each name.
An `all` entry, or no `--tools`, SHALL list every tool. An unimplemented name SHALL make the
server exit with status 2 before serving.

#### Scenario: Unknown tool name fails loud
- **WHEN** `cgraph-mcp --tools graph_nope` starts
- **THEN** it prints the unknown name and exits with status 2

#### Scenario: All lists every tool
- **WHEN** `cgraph-mcp --tools "all,graph_query"` answers `tools/list`
- **THEN** it lists every tool the server implements

### Requirement: Claude Code plugin keeps its always-loaded footprint small
The Claude Code plugin SHALL list `graph_query`, `graph_context`, `graph_impact` and
`graph_status` unless `CGRAPH_MCP_TOOLS` names others, and its `cgraph-enrich` skill SHALL NOT be
model-invocable.

#### Scenario: Plugin lists the core tools
- **WHEN** Claude Code starts with the plugin installed and `CGRAPH_MCP_TOOLS` unset
- **THEN** the session lists exactly the four core cgraph tools, and its first request's prefix
  is at most 150 tokens larger than the same session without the plugin

### Requirement: MCP server prewarms a repository
When `cgraph-mcp` starts for a project root that is a git repository (`.git` as a directory, or a
file in a worktree) or holds `cgraph-out/`, it SHALL start the project's daemon in the background
without waiting for it. For any other root it SHALL NOT start a daemon until a tool is called.

#### Scenario: Repository
- **WHEN** `cgraph-mcp` starts for a git repository root and receives only `initialize`
- **THEN** a daemon for that root is running within seconds, and `git status` in the root shows
  nothing new

#### Scenario: Not a repository
- **WHEN** `cgraph-mcp` starts for a root with neither `.git` nor `cgraph-out/`
- **THEN** no daemon starts and the root gains no `cgraph-out/`
