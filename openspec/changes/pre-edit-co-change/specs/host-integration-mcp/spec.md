## MODIFIED Requirements

### Requirement: A pre-edit hook warns before an edit crosses a service
The system SHALL ship a Claude Code `PreToolUse` hook for `Edit`, `Write` and `MultiEdit` that, given the hook's input, finds the edited file's repository (the nearest `.git`, or the workspace member holding the file when a workspace lists the member and not the repository) and prints `hookSpecificOutput.additionalContext` with up to two sections. The first, headed `cgraph: files likely to change with <file>`, SHALL list up to five files from the daemon's `co-change` op for the lines the edit replaces (the whole file for a `Write`), one line each as `<path>: <reason>`. The second, inside a workspace only, SHALL list each other service behind an endpoint the file serves or calls. The whole context SHALL stay within 800 tokens (about 4 characters per token); files that do not fit are left out. When the file's repository is still building, or another repository cannot answer, the services section SHALL say so rather than stay silent, and SHALL NOT claim the edit crosses into other services unless a caller or handler was found; the files section SHALL be left out while the graph is still building, when the daemon cannot answer or does not know the file, and when an edit's text is not in the file. It SHALL print nothing when neither section has anything to say or the input cannot be read; it SHALL always exit 0 and SHALL NOT block the edit; it SHALL wait for cold daemons no longer than `CGRAPH_HOOK_WAIT_MS` (default 3000 ms); and like the reference hook it SHALL call the thin client and start no language runtime.

#### Scenario: Editing a routes file surfaces its callers
- **WHEN** the hook receives an `Edit` of a file declaring `GET /api/v1/stats`, which another repository's `loadStats` fetches
- **THEN** its output is a `PreToolUse` `additionalContext` naming that route and `loadStats`

#### Scenario: Editing a function names its callers first
- **WHEN** the hook receives an `Edit` of a line inside `loadStats`, which `src/page.ts` calls
- **THEN** its output opens with `cgraph: files likely to change with src/stats.ts` and lists `src/page.ts: calls `loadStats``, followed by the services section

#### Scenario: A repository in no workspace still gets the files
- **WHEN** the hook receives a `MultiEdit` of a function in a repository no workspace lists, called from `src/use.ts` and tested by `src/helper.test.ts`
- **THEN** its output lists both files within 800 tokens and says nothing about services

#### Scenario: A plain file is silent
- **WHEN** the hook receives an `Edit` of a file that serves and calls no endpoint and is joined to no other file
- **THEN** it prints nothing and exits 0

#### Scenario: A repository still building is named
- **WHEN** the hook receives an `Edit` of a file whose repository's daemon is still building
- **THEN** its output says that repository is still building and does not claim a crossing
