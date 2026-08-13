## ADDED Requirements

### Requirement: Snapshot-bound source evidence
Every source snippet returned by a graph read carrying `expected_content_root` SHALL be sliced from bytes whose SHA-256 matches the source hash recorded in the selected immutable graph snapshot. The daemon SHALL read and hash each source file at most once per request and SHALL return an error without partial graph data when required source evidence cannot be verified.

#### Scenario: Pinned snippet matches selected snapshot
- **WHEN** a client synchronizes, supplies the returned content root to a snippet-producing read, and the source bytes are unchanged
- **THEN** the read returns the snippet together with its source SHA-256 and the selected content root

#### Scenario: Working tree changes before watcher publication
- **WHEN** source bytes change after synchronization but before a newer graph snapshot is published and a client performs a pinned context, explain, or entity read
- **THEN** the daemon returns a source-snapshot-mismatch error, returns no graph result, and instructs the client to synchronize again

#### Scenario: Required source evidence is absent
- **WHEN** a pinned read would return a snippet for a node whose selected snapshot has no source hash or whose file cannot be read
- **THEN** the daemon fails the read without returning an unverified or empty substitute snippet

#### Scenario: Dense same-file response is internally consistent
- **WHEN** a pinned context response includes multiple symbols from one source file while another process rewrites that file
- **THEN** every returned snippet is sliced from one request-local verified buffer or the entire read fails

#### Scenario: Persisted fast-load preserves snippet verification
- **WHEN** a daemon fast-loads a graph whose manifest was content-verified and a client performs a pinned snippet read
- **THEN** the daemon reconstructs the source hash ledger from the manifest and verifies the snippet without re-extracting the project

### Requirement: Source verification preserves graph parity and bounded cost
Snapshot source evidence SHALL remain runtime metadata outside graph topology and exports. Verification SHALL hash only files whose snippets are requested, once per request, and SHALL NOT trigger full-project verification for an ordinary graph read.

#### Scenario: Graph exports are unchanged
- **WHEN** the same project is built before and after snapshot source evidence is enabled
- **THEN** deterministic node, edge, hyperedge, ID, and graph JSON parity remains byte-identical

#### Scenario: Pinned context reads only selected source files
- **WHEN** a pinned context response selects snippets from K distinct files in a larger repository
- **THEN** source verification reads and hashes exactly those K files and does not scan unrelated project files
