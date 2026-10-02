## ADDED Requirements

### Requirement: Cypher files are detected and indexed
`detect_language` SHALL classify `.cypher` files as `Cypher` (display name `cypher`) and leave `.cql` (also Cassandra's CQL) `Unknown`, which SHALL have a registered non-grammar extractor emitting one `file` node per file plus the label contracts the file uses, so Cypher files never appear in `unextracted`.

#### Scenario: A Cypher script becomes a file node
- **WHEN** `scripts/cypher/setup.cypher` is detected and extracted
- **THEN** it is `Cypher` (and `schema.cql` is `Unknown`), the graph holds a `file` node for it, and that node `CONSUMES` the labels its live statements use
