## ADDED Requirements

### Requirement: An enrichment node's identity includes the file it came from
A node of an enrichment kind that records a `source_file` -- `document` and `media`, matched case-insensitively because `kind` is unvalidated host input -- SHALL NOT be merged by label similarity with ANY node whose `source_file` differs, enrichment or code symbol alike. A document's identity is scoped by the file it was written from: unlike a `file` node (which is excluded from dedup entirely), a document still participates in dedup within its own file.

A `concept` -- or any enrichment node that omits `source_file` -- is unaffected: with no file recorded, label similarity remains its only merge signal.

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
