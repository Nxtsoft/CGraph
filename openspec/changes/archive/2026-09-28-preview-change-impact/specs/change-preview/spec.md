## ADDED Requirements

### Requirement: Preview a proposed diff without publishing
The daemon SHALL expose a `preview` op (MCP tool `graph_preview`) that accepts a unified diff as text and returns the symbol changes it makes and the dependents of those changes, computed on a private branch graph. The op SHALL NOT publish the branch or alter the served snapshot. It SHALL NOT create or modify any file under the project root, except new obligation sidecars under `cgraph-out/memory/` when `record` is true.

#### Scenario: Deleting an exported function lists its callers
- **WHEN** `preview` is called with a diff that deletes a function other files call
- **THEN** the response lists the function as deleted and its callers as base-side dependents, with witness edges

#### Scenario: The served graph is untouched
- **WHEN** a `preview` without `record` completes or fails
- **THEN** the served snapshot pointer and content root are unchanged, and no file under the project root is created or modified

### Requirement: The preview refuses when the index cannot answer
The op SHALL run through a handler injected by the daemon that owns the file index. It SHALL return the typed error `building` without blocking while a build holds the index, and the typed error `index_not_hydrated` when the index holds no extractions (after a fast-load restart, before the first rebuild).

#### Scenario: Preview right after a fast-load restart
- **WHEN** the daemon fast-loaded its persisted graph and has not rebuilt yet
- **THEN** `preview` returns `index_not_hydrated` and no symbol changes or dependents

#### Scenario: Preview during a full build
- **WHEN** a full build holds the index lock
- **THEN** `preview` returns `building` immediately

### Requirement: The preview validates the diff against the source
For every touched file the graph indexes, the op SHALL require each hunk's old side to match the live file bytes, and the file's current hash to equal the served snapshot's `source_hashes` entry. For a touched file the graph does not index, it SHALL require only the old side to match the live bytes, and SHALL report the file under `unassessed`. The op SHALL reject diffs that touch the root `.gitignore`, `tsconfig.json` or `jsconfig.json`, or that exceed 1 MiB or 50 files. Every rejection SHALL be a typed error with no partial result.

#### Scenario: A hunk does not match the file
- **WHEN** a hunk's old lines differ from the file on disk
- **THEN** the op returns `mismatch` and no symbol changes or dependents

#### Scenario: An indexed file was edited outside the hunk after extraction
- **WHEN** a touched indexed file was edited, outside the hunk's lines, after the served snapshot extracted it
- **THEN** the op returns `not_caught_up`, telling the caller to run `graph_update`

#### Scenario: A build file rides along
- **WHEN** a diff touches `CMakeLists.txt` and one indexed source file
- **THEN** the preview succeeds, reports `CMakeLists.txt` under `unassessed`, and does not re-extract it

### Requirement: Base and branch come from one index view
The op SHALL build both the base and the branch from the daemon's cached per-file extractions. Both SHALL be code-only, with no semantic drops or memory nodes, and fully deduplicated. The branch SHALL re-extract only the touched indexed files, from their proposed contents in memory. An added file SHALL obey the same detection rules as a scan (language, gitignore, dependency directories, 8 MiB cap), and otherwise be reported as skipped. The response SHALL report `files_reextracted` and `files_cache_hit`.

#### Scenario: A one-file diff re-extracts one file
- **WHEN** a diff touches exactly one indexed file
- **THEN** `files_reextracted` is 1 and every other indexed file is a cache hit

#### Scenario: An added gitignored file is not extracted
- **WHEN** a diff adds a file under a gitignored directory
- **THEN** the file is reported as skipped and contributes no nodes to the branch

### Requirement: Preview agrees with change_context
For the same file set and diff, the preview's `changes` and impacted node ids SHALL equal those produced by `change_context`'s evidence builder over a materialized copy with the diff applied, with both graphs built as `rebuild_graph` followed by full dedup.

#### Scenario: Parity after the served graph has drifted
- **WHEN** the daemon has applied an incremental update and holds a semantic drop and a checkpoint, and a fixture diff is previewed and separately materialized
- **THEN** both return identical `changes` and identical impacted node ids

### Requirement: Path checks run on the branch
`preview` SHALL accept optional `paths`, pairs of node ids or names. For each pair it SHALL report whether a dependency path exists on the base and on the branch.

#### Scenario: A change severs a path
- **WHEN** a diff removes the only call connecting A to B, and `paths` contains [A, B]
- **THEN** the response reports the path present on the base and absent on the branch

### Requirement: Preview is bounded and observable
The op SHALL accept `max_depth` from 1 to 5 (default 3) and a `budget`, and SHALL reject out-of-range values with a typed error. The op SHALL be counted in `status.ops` and in the durable op-stats ledger. In a workspace root it SHALL return `workspace_op_unsupported`.

#### Scenario: Out-of-range depth
- **WHEN** `preview` is called with `max_depth` 9
- **THEN** it returns a typed range error and no result
