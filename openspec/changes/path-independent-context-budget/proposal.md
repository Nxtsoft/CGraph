## Why

`context` charges each entry's serialized JSON against the token budget, and that JSON carried the
absolute `source_file`. The same tree therefore packed fewer entries the deeper it was checked out:
with one query and one snapshot, an 89-character root packed 40 entries and a 147-character root
packed 38 (bench/arb/RESULTS.md, CGR-24); a full ARB run from a longer root changed 181 of 427
top-20 lists (CGR-33). Node ids stopped carrying the root in relative-node-ids; the paths in a
context entry still did.

Charging a root-independent size while still emitting the absolute path would only move the
problem: the budget would no longer match what the agent receives. So the entries change instead.

## What Changes

- A `context` entry's `source_file` (focus and every `included` row) is the file's path relative
  to the daemon's project root. The response states the root once, as a top-level `root`, outside
  the entries the budget is charged for. A file outside the root keeps its recorded path.
- The budget is charged for exactly what is emitted, so the same tree packs the same entries, in
  the same order, at the same `tokens_used`, at any root.
- The relativization is the one `report` already uses for its rows (`relative_file`, now shared
  from `report.hpp`); context follows the same convention as report rows and `change-context`
  `path` fields, which were already repo-relative.
- `change-context` packs its context with each file relative to the root of the snapshot it came
  from, so where the base and head checkouts sit no longer changes what fits. Its emitted `path`
  field is unchanged.
- Unchanged: `graph.json` and `Node::source_file` stay absolute (the daemon reads files through
  them); `query`, `explain`, `impact` and `path` briefs keep absolute paths. No persisted graph
  content changes, so the index key is not bumped.

## Consumers checked

- MCP `graph_context` forwards the daemon result verbatim (`mcp_server.cpp`).
- `context` is never federated across a workspace (`federate_workspace_request` answers it per
  repository), so a relative path is never ambiguous between repos.
- `bench/arb/cgraph_eval.py` mapped context paths to repo files by stripping the root prefix; it
  now reads context paths as already relative.
- `integrations/skills/cgraph/SKILL.md` documents the relative paths and the `root` field.

## Impact

Agents get a shorter, checkout-independent bundle; a caller that joined `source_file` across ops
must resolve a context path against `root`. `cgraph_retrieval_quality_test` now serves its graph
with the project root set, as the daemon does, and is re-pinned to the higher recall that results.
