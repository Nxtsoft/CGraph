## ADDED Requirements

### Requirement: Query search hits carry source
The `query` op's search route SHALL attach the source location and source snippet (with
`source_sha256`, capped at 40 lines and 2000 characters like every other snippet) to its first
three hits that are neither `file` nodes nor enrichment nodes. Other hits SHALL stay briefs.

#### Scenario: A search answers with code
- **WHEN** a query matches several symbols by substring and the top one is a class on disk
- **THEN** that hit's `snippet` starts at the class declaration and its `location.start_line` is
  the declaration line
