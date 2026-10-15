## Why

CGR-29 (PR #190) measured the Claude Code plugin on 32 SWE-bench Verified tasks: billed cost was
7.1% higher with it installed, and the agent called a CGraph tool in 0 of 96 runs. Claude Code
names every listed MCP tool and every model-invocable skill description in every request, so the
plugin added about 751 tokens that were written to the prompt cache and re-read on every turn.
When the agent was told to use CGraph, its first `graph_query` waited 3 to 10 seconds for the
graph, and a search answer held only names and lines, so it re-read the files anyway.

## What Changes

- `cgraph-mcp --tools a,b,c` limits what `tools/list` advertises (`all`, or no flag, lists every
  tool; whitespace around names is ignored; an unknown name exits 2). The plugin's `.mcp.json` passes
  `${CGRAPH_MCP_TOOLS:-graph_query,graph_context,graph_impact,graph_status}`, so a user can list more.
- The `cgraph` skill description is one short line; `cgraph-enrich` sets
  `disable-model-invocation: true` and runs only as `/cgraph-enrich`.
- `cgraph-mcp` starts the project's daemon when it starts, only when `<root>/cgraph-out/` already
  exists, so a previously indexed project answers its first call from a loaded graph and an
  unindexed repository is never written to.
- The `query` op's search route attaches the source span (location, `snippet`, capped as
  everywhere else at 40 lines / 2000 chars) to its first three code hits; file and enrichment
  hits stay briefs.
- `bench/billed-cost/prefix_probe.py` measures an arm's always-loaded prefix; `run_ab.py
  templates --marketplace` installs the plugin from a local checkout.

## Impact

- Measured prefix delta with the plugin: +753 tokens before, +133 after with the four default tools (+111 with three; `prefix_probe.py`).
- Hosts other than the plugin are unchanged unless they pass `--tools`.
