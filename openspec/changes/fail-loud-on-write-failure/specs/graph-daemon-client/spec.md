## MODIFIED Requirements

### Requirement: Persistence never destroys the last-known-good graph
The daemon SHALL persist the graph snapshot by writing a temp file and atomically renaming it
over `graph.json` only after the whole snapshot was written: the write, flush and close SHALL
succeed and the temp file SHALL hold exactly the snapshot's bytes. On a failed write or a failed
rename it SHALL leave the existing `graph.json` untouched, remove the temp file, and surface the
failure. It SHALL NOT delete the existing file to retry.

#### Scenario: Rename fails
- **WHEN** the atomic rename of the temp snapshot fails
- **THEN** the prior `graph.json` still exists with its previous content and the temp file is
  cleaned up

#### Scenario: The disk fills during the write
- **WHEN** the temp snapshot cannot be written whole (the disk or the file-size limit runs out
  part-way)
- **THEN** persistence reports failure, the prior `graph.json` still exists with its previous
  content, and no temp file is left behind
