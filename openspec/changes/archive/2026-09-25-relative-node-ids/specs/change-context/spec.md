## ADDED Requirements

### Requirement: Symbol changes can be requested without impact or context evidence
The operation SHALL accept a `symbols_only` mode (CLI `--symbols-only`, MCP `symbols_only: true`)
that returns every `changes[].symbol_changes` entry for the supplied diff and produces no impact
and no context evidence. In this mode the budget SHALL NOT shed any part of the response, and
source verification SHALL still run before the response is returned.

#### Scenario: Every symbol change survives a starving budget
- **WHEN** `symbols_only` is set with a budget that would shed impacts in the default mode
- **THEN** `changes` is identical to the default mode's `changes`, `impacts` and `context` are
  empty, `omitted.impacts` and `omitted.context` are zero, and `truncated` is false

#### Scenario: Mode is echoed and shared by every surface
- **WHEN** the operation runs through the CLI or the MCP tool with `symbols_only`
- **THEN** the response carries `symbols_only: true` and the same `changes` as the engine call
