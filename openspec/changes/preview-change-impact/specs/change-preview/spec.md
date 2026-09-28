## ADDED Requirements

### Requirement: Preview a proposed diff without writing or publishing
The daemon SHALL expose a `preview` op (MCP tool `graph_preview`) that accepts a unified diff as text and returns the symbol changes it makes and the dependents of those changes, computed on a private branch graph. The op SHALL NOT write any file, and SHALL NOT publish the branch or alter the served snapshot.

#### Scenario: Deleting an exported function lists its callers
- **WHEN** `preview` is called with a diff that deletes a function other files call
- **THEN** the response lists the function as deleted and its callers as base-side dependents, with witness edges

#### Scenario: The served graph is untouched
- **WHEN** any `preview` completes or fails
- **THEN** the served snapshot, its content root, and every file under the project root are unchanged

### Requirement: The preview's old side must match the extracted source
The op SHALL validate every hunk's old side against the live file bytes, and SHALL require each touched file's current hash to equal the served snapshot's `source_hashes` entry. It SHALL reject the whole operation otherwise, returning no partial result.

#### Scenario: A hunk does not match the file
- **WHEN** a hunk's old lines differ from the file on disk
- **THEN** the op returns an error and no symbol changes or dependents

#### Scenario: The graph has not caught up with an edit
- **WHEN** a touched file was edited after the served snapshot extracted it
- **THEN** the op returns an error telling the caller to run `graph_update`

### Requirement: The branch is rebuilt from the warm cache
The branch graph SHALL be built from the daemon's cached per-file extractions, re-extracting only the files the diff touches, from their proposed contents in memory. The response SHALL report `files_reextracted` and `files_cache_hit`.

#### Scenario: A one-file diff re-extracts one file
- **WHEN** a diff touches exactly one file
- **THEN** `files_reextracted` is 1 and every other indexed file is a cache hit

### Requirement: Preview agrees with change_context
For the same base and diff, the preview's `changes` and impacted node ids SHALL equal those of `change_context` run with the target tree materialized on disk.

#### Scenario: Parity with a materialized target
- **WHEN** a fixture diff is previewed, and separately applied to a copy of the base and passed to `change_context`
- **THEN** both return identical `changes` and identical impacted node ids

### Requirement: Path checks run on the branch
`preview` SHALL accept optional `paths`, pairs of node ids or names. For each pair it SHALL report whether a dependency path exists on the base and on the branch.

#### Scenario: A change severs a path
- **WHEN** a diff removes the only call connecting A to B, and `paths` contains [A, B]
- **THEN** the response reports the path present on the base and absent on the branch
