## ADDED Requirements

### Requirement: Canonical code content root
The deterministic graph pipeline SHALL compute a `sha256-merkle-v1` root from one leaf per detected code file, where each leaf binds the normalized project-relative path to that file's SHA-256 content hash. Root construction SHALL be deterministic and independent of input enumeration order.

#### Scenario: Identical source trees have identical roots
- **WHEN** the same path-and-content entries are supplied in different orders
- **THEN** the pipeline returns the same root and leaf count

#### Scenario: Source identity changes with relevant inputs
- **WHEN** file content, normalized path, addition, or deletion changes
- **THEN** the pipeline returns a different root

#### Scenario: Empty source tree has a stable identity
- **WHEN** no detected code files exist
- **THEN** the pipeline returns the domain-separated empty-tree root with a zero leaf count

### Requirement: Content-verified code rescan
A content-verified code rescan SHALL hash every currently detected code file, SHALL NOT accept a metadata-only cache hit, and SHALL reuse an existing extraction only when the freshly computed content hash matches that extraction's stored hash.

#### Scenario: Equal-length preserved-mtime edit is invalidated
- **WHEN** a code file is overwritten with different equal-length bytes and its original modification time is restored
- **THEN** a content-verified rescan computes a different file hash, re-extracts that file, and publishes a different content root

#### Scenario: Timestamp-only rewrite reuses extraction
- **WHEN** file metadata changes but freshly computed content is byte-identical
- **THEN** the rescan records a content-hash hit and reuses the existing extraction

#### Scenario: Verified rescan removes vanished input
- **WHEN** a previously indexed code file is absent from the verified source tree
- **THEN** its extraction and leaf are removed before the new graph and root are published
