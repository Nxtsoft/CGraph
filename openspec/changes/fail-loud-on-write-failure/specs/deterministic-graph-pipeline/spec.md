## ADDED Requirements

### Requirement: Output files are written whole or not at all
Every file the `cgraph` CLI writes (the one-shot exports and `stats.json`, the enrichment plan
manifest, stat index and cache, `seam gen` / `seam discover` fragments, `seam fuse` `graph.json`,
`graph.html` and its `.cgraph-seam` marker, and the `workspace init` manifest) SHALL be written to
a temp file in the same directory and renamed into place only after the write, flush and close
succeeded and the temp file holds exactly the intended bytes. When a file cannot be written whole,
the command SHALL exit non-zero with a message naming that file and the reason, SHALL leave any
prior file at that path untouched, and SHALL leave no temp file behind. Each write SHALL use its
own temp file, so two writers of one path (two processes, or two threads) never mix their bytes and
neither fails because of the other. Because the file is replaced by rename, a symlink at the path is
replaced by a regular file rather than written through, and the new file's mode is the umask
default rather than the prior file's.

#### Scenario: seam fuse on a full disk
- **GIVEN** an output directory that already holds a `graph.json`
- **WHEN** `cgraph seam fuse` runs and the disk fills while it writes the fused `graph.json`
- **THEN** it exits non-zero naming `<out>/graph.json`, the prior `graph.json` is unchanged, and
  neither `graph.json.tmp` nor `.cgraph-seam` is written

#### Scenario: One-shot build on a full disk
- **WHEN** `cgraph --root ROOT --out OUT` runs and the disk fills while it writes `graph.html`
- **THEN** it exits non-zero naming `OUT/graph.html` and no `graph.html` or `graph.html.tmp`
  exists in `OUT`

#### Scenario: Two writers of one file
- **WHEN** two processes replace the same output file at the same time, each with its own contents
- **THEN** both writes succeed and the file afterwards holds exactly one writer's complete contents

#### Scenario: Output is unchanged when writes succeed
- **WHEN** the same commands run with space to spare
- **THEN** every output file is byte-identical to what the previous direct writes produced
