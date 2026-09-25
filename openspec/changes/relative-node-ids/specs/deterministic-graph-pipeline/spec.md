## ADDED Requirements

### Requirement: Node identifiers are independent of the project's location on disk
Node identifiers SHALL be derived from the source path **relative to the project root**, so that
extracting the same source tree from two different absolute locations produces byte-identical
identifiers. Identifiers SHALL NOT contain any path segment above the project root.

This constrains the *input* given to the identifier normalizer. It does not alter the
normalization itself, which remains governed by `ID normalization parity`.

#### Scenario: The same tree at two paths yields the same ids
- **WHEN** an identical source tree is extracted from two different absolute roots
- **THEN** the two graphs contain byte-identical node identifiers

#### Scenario: No identifier carries the machine's directory layout
- **WHEN** a graph is built from any project root
- **THEN** no node identifier contains a path segment above that root

#### Scenario: Normalization behavior is unchanged
- **WHEN** the identifier normalizer runs against the Unicode identifier fixtures
- **THEN** every output still matches the Graphify reference exactly

### Requirement: Retrieval budget is not spent on the project's location
No node identifier in a context entry SHALL include any portion of a path above the project
root, so that the identifier part of every entry's cost is the same however deeply the project
is checked out. (A brief's `source_file` remains the absolute path the daemon reads from; it is
not an identifier and is not covered by this requirement.)

#### Scenario: Identifier cost does not move with checkout depth
- **WHEN** the same tree is built from two roots of different length
- **THEN** every node identifier, and therefore every identifier's serialized cost, is
  byte-identical between the two builds

### Requirement: Identifier-keyed persisted state is invalidated, never mis-resolved
The system SHALL treat identifier-keyed persisted state written under a previous identifier
scheme as invalid and requeue it, and SHALL NOT resolve an old identifier to a different node.

#### Scenario: A stale semantic cache record is rejected
- **WHEN** a semantic cache record references a node identifier written under the previous scheme
- **THEN** the record is marked invalid with its missing dependency named, and its source is
  requeued

#### Scenario: A stale persisted extraction index is not loaded
- **WHEN** the persisted fast-load index was written under the previous identifier scheme
- **THEN** startup treats it as unusable and performs a verified rebuild
