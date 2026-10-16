## ADDED Requirements

### Requirement: The project output directory ignores itself
When the daemon starts for a project root it SHALL create `<root>/cgraph-out/` and, if that
directory has no `.gitignore`, SHALL write one that ignores every entry in it. It SHALL NOT rewrite
an existing `.gitignore` there.

#### Scenario: Daemon starts in a repository
- **WHEN** a daemon starts for a git repository root that has no `cgraph-out/`
- **THEN** `cgraph-out/.gitignore` exists and `git status --porcelain` lists nothing under
  `cgraph-out/`

#### Scenario: Edited ignore file is kept
- **WHEN** `cgraph-out/.gitignore` already exists with other content and the daemon starts
- **THEN** the file is unchanged
