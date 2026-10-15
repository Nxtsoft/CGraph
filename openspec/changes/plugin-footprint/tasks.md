# Tasks

## 1. Footprint

- [x] 1.1 `cgraph-mcp --tools`; `handle_mcp_request` takes `listed_tools`; `mcp_tool_names()`.
- [x] 1.2 Plugin `.mcp.json` lists the core three via `CGRAPH_MCP_TOOLS`; short `cgraph` skill
      description; `cgraph-enrich` not model-invocable.
- [x] 1.3 `prefix_probe.py`: +753 tokens before, +111 after.

## 2. First answer

- [x] 2.1 Prewarm when `cgraph-out/` exists; no daemon and no writes otherwise.
- [x] 2.2 Search route attaches source to the first three code hits.

## 3. Measure

- [ ] 3.1 Re-run the CGR-29 A/B (same tasks, seeds, binary, model) on this build.
