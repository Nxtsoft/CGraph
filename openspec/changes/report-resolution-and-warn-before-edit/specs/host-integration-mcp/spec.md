## ADDED Requirements

### Requirement: A pre-edit hook warns before an edit crosses a service
The system SHALL ship a Claude Code `PreToolUse` hook for `Edit`, `Write` and `MultiEdit` that, given the hook's input, finds the edited file's repository and prints `hookSpecificOutput.additionalContext` listing each other service behind an endpoint the file serves or calls. When the file's repository is still building, or another repository cannot answer, it SHALL say so rather than stay silent, and SHALL NOT claim the edit crosses into other services unless a caller or handler was found. It SHALL print nothing when the file is outside a workspace, touches no such endpoint, or the input cannot be read; it SHALL always exit 0 and SHALL NOT block the edit; it SHALL wait for cold daemons no longer than `CGRAPH_HOOK_WAIT_MS` (default 3000 ms); and like the reference hook it SHALL call the thin client and start no language runtime.

#### Scenario: Editing a routes file surfaces its callers
- **WHEN** the hook receives an `Edit` of a file declaring `GET /api/v1/stats`, which another repository's `loadStats` fetches
- **THEN** its output is a `PreToolUse` `additionalContext` naming that route and `loadStats`

#### Scenario: A plain file is silent
- **WHEN** the hook receives an `Edit` of a file that serves and calls no endpoint
- **THEN** it prints nothing and exits 0

#### Scenario: A repository still building is named
- **WHEN** the hook receives an `Edit` of a file whose repository's daemon is still building
- **THEN** its output says that repository is still building and does not claim a crossing
