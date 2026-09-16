## ADDED Requirements

### Requirement: One-shot builds write the design report instead of call-flow.html

`write_exports` SHALL write `design.mmd` (the design view's `flowchart TD`) and `design.md` (its markdown) for the whole project with no budget, and SHALL NOT write `call-flow.html`. `graph.json` and every other export SHALL be unchanged.

#### Scenario: Export set after a one-shot build
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/design.mmd` and `DIR/design.md` exist and `DIR/call-flow.html` does not
