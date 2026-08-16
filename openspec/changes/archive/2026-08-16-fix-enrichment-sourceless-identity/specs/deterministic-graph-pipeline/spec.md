## MODIFIED Requirements

### Requirement: An enrichment node's identity includes the file it came from
A node of an enrichment kind -- `document` and `media`, matched case-insensitively because `kind` is unvalidated host input -- SHALL NOT be merged by label similarity with ANY node unless both nodes share the SAME non-empty `source_file`. A document's identity is scoped by the file it was written from: a differing file keeps them apart, a shared non-empty file may still merge (a genuine re-extraction), and a MISSING file is no proof of shared identity and also keeps them apart. This holds against any counterpart, enrichment or code symbol alike; unlike a `file` node (excluded from dedup entirely), a document still participates in dedup within its own file.

A `concept` -- which is not a file-scoped kind -- is unaffected: with no file to scope to, label similarity remains its only merge signal. A `document` or `media` node that omits `source_file` is NOT treated like a concept: it is kept apart rather than label-merged, so a non-compliant host that drops `source_file` does not have its similar-named documents silently deleted.

#### Scenario: A change's proposal, tasks and design stay three documents
- **GIVEN** document nodes for `x/proposal.md`, `x/tasks.md` and `x/design.md`
- **AND** their labels are similar enough to cross the fuzzy threshold
- **WHEN** semantic dedup runs
- **THEN** all three nodes survive
- **AND** every edge that referenced them still resolves

#### Scenario: Two records of one document still merge
- **GIVEN** two document nodes with the same non-empty `source_file`
- **WHEN** semantic dedup runs
- **THEN** they merge into one node

#### Scenario: Source-file-less documents are not merged by label
- **GIVEN** two `document` nodes with similar labels, a shared community, and no `source_file`
- **WHEN** semantic dedup runs
- **THEN** both survive

#### Scenario: Concept dedup is unchanged
- **GIVEN** two `concept` nodes with the same label and no `source_file`
- **THEN** they merge into one node
