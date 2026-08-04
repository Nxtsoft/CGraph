## ADDED Requirements

### Requirement: Content-verified graph synchronization
The daemon `update` operation SHALL act as a blocking content-verification barrier for detected code inputs. A successful response SHALL identify the immutable graph snapshot produced or confirmed by that barrier with its content root, algorithm, leaf count, and verification work counts.

#### Scenario: Synchronization catches preserved metadata
- **WHEN** a source file changes to different equal-length bytes while retaining its prior modification time and the client invokes `update`
- **THEN** the daemon re-extracts the changed file and returns a new content root

#### Scenario: Clean synchronization preserves identity
- **WHEN** the client invokes `update` twice without source changes
- **THEN** both successful responses return the same content root and the second update reuses every extraction

#### Scenario: Update completes before returning
- **WHEN** an update response returns successfully
- **THEN** subsequent reads pinned to its content root are served from the synchronized immutable snapshot

### Requirement: Content-root-pinned graph reads
Query, path, explain, impact, and context operations SHALL accept an optional `expected_content_root`. Each successful graph read SHALL report the content root of the immutable snapshot it used.

#### Scenario: Expected root matches
- **WHEN** a graph read supplies the root returned by the latest completed update and that snapshot remains current in the daemon
- **THEN** the daemon serves the read and echoes that root in freshness metadata

#### Scenario: Expected root does not match
- **WHEN** a graph read supplies a root different from the immutable snapshot selected for the request
- **THEN** the daemon returns an error and no graph result

#### Scenario: Unpinned read remains compatible
- **WHEN** an existing client omits `expected_content_root`
- **THEN** the daemon serves the read and includes the selected snapshot root without claiming that the mutable worktree was verified at request time

### Requirement: Content-root-gated persisted fast-load
The daemon SHALL persist the source content root in the versioned index manifest and SHALL fast-load a persisted graph only after hashing the complete detected code tree and matching its file set, individual hashes, root, leaf count, and index logic version.

#### Scenario: Unchanged tree fast-loads
- **WHEN** the persisted manifest version and root match a newly content-verified source tree
- **THEN** the daemon loads the persisted graph without re-extracting source files and assigns the verified root to the live snapshot

#### Scenario: Preserved-mtime edit rejects fast-load
- **WHEN** source bytes differ from the persisted leaf while size and modification time remain equal
- **THEN** startup rejects the persisted graph and rebuilds from source

#### Scenario: Old or incomplete manifest rebuilds
- **WHEN** the manifest lacks the current logic version or a valid content root
- **THEN** startup treats it as unusable and performs a verified rebuild

### Requirement: Watcher updates remain explicitly eventual
The live watcher SHALL use filesystem metadata only to identify candidate changes, SHALL content-hash every delivered code candidate before cache reuse, and SHALL never represent watcher-only freshness as a full-tree verification.

#### Scenario: Restored-mtime local rewrite is observed
- **WHEN** a normal POSIX file rewrite changes device, inode, or change time while restoring size and modification time
- **THEN** the watcher delivers a modified event and the updater validates content before reuse

#### Scenario: Status distinguishes identity from current-worktree proof
- **WHEN** status or an unpinned read reports a snapshot content root
- **THEN** the response describes the graph's source identity without asserting that no unseen filesystem edit occurred after the last barrier
