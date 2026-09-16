## MODIFIED Requirements

### Requirement: Detection excludes dependency and virtual-environment trees
Project file detection SHALL skip dependency, build, tooling, test-runner-output, virtual-environment, agent-tooling-config, and linked git-worktree directory trees rather than index their contents as project source. A directory SHALL be excluded when its name is in the skip list — which includes the Python ecosystem (`.venv`, `venv`, `site-packages`, `__pycache__`, `.tox`, `.nox`, `.pytest_cache`, `.mypy_cache`, `.ruff_cache`, `.hypothesis`, `.eggs`), the agent-CLI / spec-tool config directories (`.claude`, `.codex`, `.gemini`, `.cursor`, `.factory`, `.opencode`, `.windsurf`, `.aider`, `.specify`), and the test-runner output directories (`playwright-report`, `test-results`, `blob-report`) — OR when it contains a `pyvenv.cfg` virtual-environment marker — OR when it is a git-worktree checkout tree. A worktree checkout SHALL be detected by either of two structural markers: (a) a `.git` entry that is a regular file (the live worktree `gitdir:` marker) rather than a directory, or (b) a directory named `worktrees` whose parent directory name begins with a dot (the `<.tool>/worktrees/` convention used by agent tools, which also covers stale checkouts whose `.git` has been pruned). The project root's own `.git` is a directory and SHALL NOT trigger this exclusion, and a `worktrees` directory under a non-dotted parent (a legitimate source module) SHALL NOT be excluded. The daemon file watcher SHALL apply the identical exclusion, so a file created under a skipped tree never produces an incremental update. The same exclusion governs the enrichment chunk planner, so neither agent-tooling docs nor worktree-duplicated docs are planned for semantic enrichment. Files that remain in scope are extracted unchanged (parity is held).

#### Scenario: Virtualenv contents are not indexed
- **WHEN** a project root contains a virtualenv (e.g. `research/.venv/lib/pythonX/site-packages/…`) and the graph is built
- **THEN** no node has a `source_file` under that `.venv` or any `site-packages` directory, and the graph contains only the project's own source

#### Scenario: Oddly-named virtualenv is detected by marker
- **WHEN** a directory not in the name skip list (e.g. `qa-env/`) contains a `pyvenv.cfg` file
- **THEN** the directory and its contents are skipped during detection

#### Scenario: Agent-tooling config directories are not indexed or enriched
- **WHEN** a project root contains agent-CLI or spec-tool config trees (e.g. `.claude/commands/`, `.factory/skills/`, `.specify/templates/`)
- **THEN** detection skips them and the enrichment planner does not plan their documents, so they contribute neither code nodes nor doc nodes

#### Scenario: Test-runner output is not indexed
- **WHEN** a project root contains Playwright output (`playwright-report/trace/*.js`, `test-results/`, `blob-report/`), whether or not `.gitignore` names it
- **THEN** no node has a `source_file` under those directories, and `report design` and `report clones` see only the project's own functions

#### Scenario: Live and stale git-worktree trees are not indexed
- **WHEN** a project root contains agentic git worktrees under the `<.tool>/worktrees/<id>/` convention, whether live (`.git` file present) or stale (`.git` pruned)
- **THEN** detection skips them, so the graph is not duplicated by checkout copies
