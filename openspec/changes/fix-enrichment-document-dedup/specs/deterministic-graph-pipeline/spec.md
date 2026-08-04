## ADDED Requirements

### Requirement: An enrichment node's identity includes the file it came from
Two nodes that were not extracted from source but do record a `source_file` -- `document` and `media` -- SHALL NOT be merged by label similarity when their `source_file` values differ. A document's identity is the file it was written from, on the same footing as a `file` node's identity being its path.

A `concept`, which records no `source_file`, is unaffected: label similarity remains its only merge signal.

#### Scenario: A change's proposal, tasks and design stay three documents
- **GIVEN** document nodes for `x/proposal.md`, `x/tasks.md` and `x/design.md`
- **AND** their labels are similar enough to cross the fuzzy threshold
- **WHEN** semantic dedup runs
- **THEN** all three nodes survive
- **AND** every edge that referenced them still resolves

#### Scenario: Two records of one document still merge
- **GIVEN** two document nodes with the same `source_file`
- **WHEN** semantic dedup runs
- **THEN** they merge into one node

#### Scenario: Concept dedup is unchanged
- **GIVEN** two `concept` nodes with the same label and no `source_file`
- **THEN** they merge into one node
