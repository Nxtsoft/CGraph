## ADDED Requirements

### Requirement: Every function node carries a rename-insensitive fingerprint

For every `function` node the extractor creates, the pipeline SHALL compute a fingerprint of the function's body subtree (the language config's body field; the whole definition when there is none): the normalized token stream in which every identifier is `ID`, every string, number or character literal is `LIT`, comments are dropped, and keywords, operators and punctuation keep their text; every run of five consecutive tokens hashed with 64-bit FNV-1a; and the winnowed selection of those hashes (window four, minimum per window, rightmost on a tie) as a sorted unique set, together with the token count. The fingerprint SHALL be stored in `Fragment::fingerprints` keyed by node id, SHALL travel with the fragment through the incremental index, SHALL be unioned into `GraphSnapshot::fingerprints` by both merge paths, and SHALL NOT appear in any fragment file, `graph.json` or other export. The computation SHALL be deterministic.

#### Scenario: Renamed copies fingerprint identically
- **GIVEN** two functions whose bodies differ only in identifier names, string literals and numeric literals
- **THEN** their fingerprints have equal shingle sets and equal token counts, and their similarity is 1.0

#### Scenario: An edited copy keeps a partial fingerprint
- **GIVEN** a copy with two statements inserted
- **THEN** its similarity to the original is below 1.0 and above 0.5

#### Scenario: Exports are unchanged
- **WHEN** a one-shot build writes `graph.json`
- **THEN** the output is byte-identical to a build without fingerprints
