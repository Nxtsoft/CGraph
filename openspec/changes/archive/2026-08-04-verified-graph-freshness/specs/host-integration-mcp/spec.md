## ADDED Requirements

### Requirement: MCP exposes verified graph synchronization
The MCP `graph_update` tool SHALL describe and forward the daemon's content-verified synchronization barrier and SHALL return its content root and verification work metadata.

#### Scenario: Host synchronizes before a freshness-sensitive read
- **WHEN** a host calls `graph_update` for the project root
- **THEN** the MCP response includes the synchronized content root that can pin subsequent graph reads

### Requirement: MCP graph reads support content-root pinning
The MCP query, path, explain, impact, and context tool schemas SHALL advertise `expected_content_root` and SHALL forward it unchanged to the corresponding daemon operation.

#### Scenario: Host reads synchronized snapshot
- **WHEN** a host supplies the root returned by `graph_update` to a graph read tool
- **THEN** the tool returns data only from the matching snapshot and includes that root in its response

#### Scenario: Host is told to resynchronize
- **WHEN** a host supplies a stale or unknown content root
- **THEN** the MCP response exposes the daemon mismatch error instead of graph data

### Requirement: Bundled skill uses the verified freshness contract
The bundled CGraph skill SHALL instruct agents to synchronize before freshness-sensitive navigation and after relevant source edits, then pin graph reads to the returned content root.

#### Scenario: Agent needs current code structure
- **WHEN** an agent must rely on CGraph as current with the worktree
- **THEN** the skill routes it through `graph_update` followed by a root-pinned graph read
