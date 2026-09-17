# change-context Specification

## Purpose
TBD - created by archiving change diff-change-context. Update Purpose after archive.
## Requirements
### Requirement: Validate supplied source changes
The operation SHALL validate supplied unified diff paths, hunk positions and old/new bytes against explicit base and target roots, without writing to either root.

#### Scenario: Hunk does not match source
- **WHEN** a hunk's old content or reconstructed new content differs from its source
- **THEN** the operation rejects without returning partial success evidence

#### Scenario: Partial diff scope
- **WHEN** source hash inventories differ at paths outside the supplied diff
- **THEN** the response lists those unassessed paths and marks whole-difference coverage false

### Requirement: Preserve snapshot-specific impact evidence
The operation SHALL analyze old ranges in the base graph and new ranges in the target graph, retaining directional witness edges and source identities for deleted symbols.

#### Scenario: Deleted export
- **WHEN** an exported function is removed from the target
- **THEN** its base callers and transitive dependents remain available with base source evidence

#### Scenario: Ambiguous symbol pairing
- **WHEN** a unique qualified declaration correspondence cannot be established
- **THEN** the response reports unresolved pairing instead of asserting a rename

### Requirement: Enforce source and response boundaries
The operation SHALL reject mismatched source pins, reverify source bytes before returning, and bound the entire operation JSON by the declared serialized-byte budget approximation.

#### Scenario: Changed source or stale pin
- **WHEN** required source evidence disagrees with a selected snapshot
- **THEN** the entire operation fails explicitly

#### Scenario: Insufficient budget
- **WHEN** mandatory metadata cannot fit within the requested budget
- **THEN** the operation fails with an explicit budget error

#### Scenario: Truncated evidence
- **WHEN** optional impact or context evidence must be omitted
- **THEN** the response reports omission counts and truncation

