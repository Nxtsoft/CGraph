## Why

Coding agents currently assemble change evidence through separate graph operations. Deleting a symbol from the target removes the old graph evidence needed to inspect its dependents. CGR-3 adds a single advisory operation over explicit base and target roots and a supplied unified diff.

## What Changes

- Validate unified diff hunks against real base and target source bytes.
- Build independent source-hashed snapshots, preserve deleted base symbols, and report partial diff scope.
- Reuse canonical directional impact traversal and a single union context gather/packer under a complete response budget.
- Expose `cgraph change-context` and `graph_change_context` without mutating roots or the resident graph.

## Capabilities

### New Capabilities
- `change-context`: Source-pinned advisory evidence for a supplied code change.

## Impact

Engine traversal and snapshot reader helpers, CLI, MCP, and real source-fixture tests. No source ID/export format change; no hosted service or safety gate. The maintenance evaluation consumes the CLI externally.
