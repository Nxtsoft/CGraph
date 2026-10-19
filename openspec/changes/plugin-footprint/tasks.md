# Tasks

## 1. Footprint

- [x] 1.1 `cgraph-mcp --tools`; `handle_mcp_request` takes `listed_tools`; `mcp_tool_names()`.
- [x] 1.2 Plugin `.mcp.json` lists the core four via `CGRAPH_MCP_TOOLS`; short `cgraph` skill
      description; `cgraph-enrich` not model-invocable.
- [x] 1.3 `prefix_probe.py`: +753 tokens before, +133 after (+111 before `graph_status` joined the default).

## 2. First answer

- [x] 2.1 Prewarm when `cgraph-out/` exists; no daemon and no writes otherwise.
- [x] 2.1a (pre-edit-live) Prewarm any git repository too; `cgraph-out/` ignores itself.
- [x] 2.2 Search route attaches source to the first three code hits.

## 3. Measure

- [x] 3.1 Re-run the CGR-29 A/B (same tasks, seeds, binary, model) on this build: cost ratio
      1.071 -> 1.006 [0.973, 1.038]; 0 CGraph calls in 96 runs, so the gate still does not pass.
