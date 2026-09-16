## ADDED Requirements

### Requirement: Report op serves a clones view

The `report` op with `view: "clones"` SHALL group functions whose fingerprints are at least `threshold` (default 0.80) Jaccard-similar into clone classes. Candidates SHALL be `function` nodes with a source file under `scope` whose fingerprint has at least `min_tokens` (default 30) tokens. Candidate pairs SHALL be gathered through an inverted index on shingle hash, skipping any shingle shared by more than 512 candidates, and each pair's Jaccard SHALL be exact. Classes SHALL be the connected components of pairs at or above `threshold`; each class SHALL carry its members (root-relative file, start and end line, label, tokens; ordered by file then line), its lowest pairwise `similarity` and its shortest member's `tokens`. A class whose members all lie under test roots SHALL be listed in `test_classes` unless `include_tests`, in which case every class is in `classes`. Classes SHALL be ordered largest first, then most similar, then longest. The response SHALL carry `totals {functions, fingerprinted, eligible, classes, test_classes, members}` and `omitted {classes, test_classes}`, and SHALL carry a `hint` naming the rescan when any in-scope function has no fingerprint. When the rendered report exceeds `budget`, whole classes SHALL be shed, test classes first, then production classes, smallest last-ranked first, and `omitted` SHALL count them. The view SHALL render `json` and `markdown`; `mermaid` and `svg` SHALL be refused with `ok: false` and `code: "report_format_unsupported"`.

#### Scenario: Identical copies form one class
- **GIVEN** two production functions with identical fingerprints of 60 tokens and a third sharing 8 of 12 shingles with them
- **WHEN** `report` is called with `view: "clones"`
- **THEN** `classes` holds one class of the two copies with `similarity` 1.0 and `tokens` 60, and the third is not a member
- **AND** at `threshold` 0.6 the third joins and the class `similarity` is 0.67

#### Scenario: The token floor excludes boilerplate
- **GIVEN** a 12-token function whose fingerprint equals the copies'
- **THEN** it is not a member at `min_tokens` 30 and is a member at `min_tokens` 10

#### Scenario: Test fixtures are their own bucket
- **GIVEN** two identical functions under `tests/`
- **THEN** they form one entry in `test_classes`, and with `include_tests` the same class appears in `classes`

#### Scenario: Missing fingerprints are reported, not hidden
- **GIVEN** a function node with no fingerprint
- **THEN** `totals.fingerprinted` is below `totals.functions` and the response carries a `hint` naming `update .`

#### Scenario: The budget sheds test classes first
- **WHEN** the budget is one token below the full report
- **THEN** the test class is omitted and the production class is kept

### Requirement: Fingerprints persist beside the graph

`persist_graph_snapshot` SHALL write the snapshot's fingerprints to `fingerprints.json` next to `graph.json`, and `load_graph_snapshot` SHALL read them back when the file is present and well-formed. A missing or unreadable sidecar SHALL load an empty fingerprint map and SHALL NOT fail the load. The index version key SHALL NOT change for this artifact.

#### Scenario: Fingerprints survive a restart
- **WHEN** a snapshot with fingerprints is persisted and loaded
- **THEN** the loaded snapshot carries the same fingerprints

#### Scenario: An older persist loads without fingerprints
- **GIVEN** a persisted `graph.json` with no `fingerprints.json`
- **THEN** the graph loads, its fingerprint map is empty, and `report clones` answers with the hint
