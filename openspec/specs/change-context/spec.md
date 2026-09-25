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

### Requirement: Symbol changes can be requested without impact or context evidence
The operation SHALL accept a `symbols_only` mode (CLI `--symbols-only`, MCP `symbols_only: true`)
that returns every `changes[].symbol_changes` entry for the supplied diff and produces no impact
and no context evidence. In this mode the budget SHALL NOT shed any part of the response, and
source verification SHALL still run before the response is returned.

#### Scenario: Every symbol change survives a starving budget
- **WHEN** `symbols_only` is set with a budget that would shed impacts in the default mode
- **THEN** `changes` is identical to the default mode's `changes`, `impacts` and `context` are
  empty, `omitted.impacts` and `omitted.context` are zero, and `truncated` is false

#### Scenario: Mode is echoed and shared by every surface
- **WHEN** the operation runs through the CLI or the MCP tool with `symbols_only`
- **THEN** the response carries `symbols_only: true` and the same `changes` as the engine call

