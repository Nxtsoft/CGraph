## ADDED Requirements

### Requirement: One-command install as a Claude Code plugin
The repository SHALL be a Claude Code marketplace whose `cgraph` plugin registers the `cgraph` MCP
server, the `cgraph` and `cgraph-enrich` skills and the pre-edit hook, with every path resolved from
`${CLAUDE_PLUGIN_ROOT}` and `${CLAUDE_PLUGIN_DATA}`. The executables SHALL come from the release
pinned in `integrations/launcher/release.json`: on first use the launcher downloads the archive for
the platform, SHALL refuse it unless its SHA-256 matches the pin, and SHALL make it visible only by
one atomic rename, so concurrent first launches share one copy and a failed download leaves nothing
cached. The launcher SHALL write nothing to stdout, which belongs to the MCP server.

#### Scenario: Install to first answer on a clean machine
- **WHEN** `claude plugin marketplace add` and `claude plugin install cgraph@cgraph` run in a clean
  HOME and a headless session asks a structure question
- **THEN** the session answers through `graph_query` within 60 seconds of the install command

#### Scenario: Tampered archive
- **WHEN** the downloaded archive's SHA-256 differs from the pin
- **THEN** the launcher exits 1 naming the mismatch and nothing is written under the cache's tag directory

### Requirement: MCP registry and npm package
`server.json` SHALL describe `io.github.Nxtsoft/cgraph` against the registry's published schema with
an npm package `cgraph-mcp` whose `mcpName` matches, and whose bins run the same launcher.

#### Scenario: npx runs the server
- **WHEN** `npx -y file:<cgraph-mcp tarball>` starts in a clean HOME and receives `initialize` and a
  `graph_query` call
- **THEN** it answers both over stdio with the graph result
