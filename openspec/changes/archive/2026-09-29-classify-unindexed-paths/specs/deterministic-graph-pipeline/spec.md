## ADDED Requirements

### Requirement: Absence from the graph is reported with its reason
The system SHALL report, for each path under the project root, whether it is indexed, deliberately
ignored, or visited but unindexed, and SHALL NOT present these as the same condition.

A deliberately ignored path means the graph is complete without it. An unindexed path means the
graph may be incomplete because of it. These carry opposite consequences for a consumer deciding
whether work can be skipped, so they SHALL be reported as separate sets.

#### Scenario: A gitignored directory is reported as ignored
- **WHEN** a directory matches the root gitignore and its contents are therefore never walked
- **THEN** the directory is reported as an ignored subtree root, and its contents are not
  enumerated

#### Scenario: An unextracted build file is reported as unindexed
- **WHEN** a file is visited, is not ignored, and no language extractor claims it
- **THEN** it is reported as unindexed, and never as ignored

#### Scenario: A detected file that produced no node is unindexed
- **WHEN** detection accepted a file but extraction produced no graph node for it
- **THEN** it is reported as unindexed rather than indexed

### Requirement: The reason report does not alter graph output
The system SHALL keep `graph.json` byte-identical when producing the reason report, carrying it as
a separate exported artifact.

#### Scenario: Graph output is unchanged
- **WHEN** a project is exported with the reason report enabled
- **THEN** `graph.json` is byte-identical to the export produced without it
