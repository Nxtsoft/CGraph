# Feature: `report modules` — a module dependency diagram for agents and humans (CGR-8)

## Why

An agent asked "what is the architecture of this repo?" today has no graph call that answers at
the package level. `graph_query` and `graph_explain` work one symbol at a time; `graph.json` is
megabytes; and `graph.html` on a 1,700-node graph paints every node in one blob, because
`analysis.cpp::write_layout` stamps igraph's unit-scale coordinates (x from −11 to 10) and the
viewer adopts them verbatim as pixels and clamps its fit zoom at 4×. So the agent falls back to
`ls` and reading files, and the human never opens the viewer twice.

The information is already in the graph: every code node carries its source file, and
`imports`/`imports_from`/`re_exports`/`CALLS` edges cross file boundaries. Grouping files by
directory and summing the edges between groups is a deterministic report the daemon can serve
in one round-trip, sized to a token budget.

## What Changes

- **A `report` daemon op** (`DaemonOp::Report`, dispatched in `daemon_ops.cpp` as a
  root-pinnable read) selected by `view` — `modules` now; `design`, `clones`, `types` reserved
  for CGR-11/10/9 and answering `code: "report_view_not_implemented"`. `report.cpp` owns
  `ReportRequest{view, format, budget, scope, include_tests, module_depth, threshold, min_tokens,
  project_root}`, `build_modules_report`, `shed_to_budget`, and the JSON / Mermaid / SVG /
  Markdown renderers. `DaemonState` gains `project_root` so module names are root-relative.
- **The modules view**: files grouped by the first `depth` directory components (default 2),
  edges aggregated with separate call and import counts, layers by longest path over the module
  DAG (layer 0 = nothing depends on it), cycles listed as strongly connected components. Test
  roots are excluded as sources unless `include_tests` — on CGraph, `tests/smoke` sends 466
  calls into `src/engine` and would own layer 0. `scope` selects which modules report their
  dependencies; their targets still appear.
- **Whole-row budgeting**: the rendered text is measured at ~4 chars/token against `budget`
  (default 6000, 0 = unlimited). Overflow sheds the lightest dependency rows first, then the
  lightest modules (an edge leaves with either endpoint). No row is ever cut; `omitted` and
  `totals` are always present.
- **Surfaces**: MCP tool `graph_report`; CLI `cgraph report <view> [--root] [--format] [--scope]
  [--depth] [--budget] [--include-tests] [--daemon]` over the thin-client runtime (spawns
  graphd); `cgraph-client ... report '{...}'`; one-shot `write_exports` writes `modules.mmd` and
  `modules.svg`. A graphd that predates the op answers `unknown op: report`, which the CLI and
  MCP turn into an "upgrade the daemon" message (CLI exit 3).
- **graph.html**: `write_layout` rescales coordinates to a canvas-sized square
  (`max(720, 30·√n)` px), and the viewer opens community-collapsed above 500 nodes — one
  super-node per community with aggregated edges, expanded by click, search, or "Expand all".

### Non-goals
- Manifest-driven grouping (package.json workspaces, go.mod, Cargo.toml, CMake targets) —
  CGR-15. Directory depth is the floor this change ships.
- The `design`, `clones`, `types` views.
- Any change to `graph.json` node/edge output. No `package` node is added; grouping is computed
  at report time.

## Impact

Measured with this branch's binaries on this branch's tree (so the counts include the change
itself; the CGR-8 acceptance numbers were taken at `origin/main` before it):

```
$ cgraph report modules --root . --depth 2 --format mermaid --scope src
  src/cli    -> src/engine   59 calls, 18 imports      (acceptance: 58, 17)
  src/daemon -> src/engine    8 calls,  6 imports      (8, 6)
  src/client -> src/engine    6 calls,  5 imports      (6, 5)
  src/mcp    -> src/engine    3 calls,  3 imports      (2, 2 — this change adds the report hint)
  src/cli    -> src/client    2 calls,  1 import
  src/mcp    -> src/client    2 calls,  1 import       (2, 1)
  layers: [src/cli, src/daemon, src/mcp] -> [src/client] -> [src/engine]; cycles: none
  report: 5 modules, 6 edges; omitted 0 modules, 0 edges (budget 6000, ~194 tokens)

$ cgraph report modules --root . --depth 2 --format mermaid --include-tests
  tests/smoke -> src/engine  466 calls, 137 imports     (acceptance: 431, 134)
  11 modules, 12 edges, ~346 tokens

$ cgraph report modules --root . --format mermaid --budget 150 --include-tests
  kept tests/smoke, src/cli, src/engine and their two heaviest edges;
  %% omitted: 8 modules, 10 edges (of 11 modules, 12 edges); ~118 tokens
```

`vendor/tree-sitter` does not appear: the detector skips vendored dependency directories, so
there is no vendor layer to rank. The graph.html first paint on the 1,708-node CGraph graph goes
from one blob to 31 labelled community discs (before/after screenshots on the PR).

- **Touches:** `src/engine/report.cpp` (+ header), `daemon_ops.cpp`, `daemon_ops.hpp`,
  `daemon_server.cpp`, `operation_stats.{hpp,cpp}`, `pipeline.{hpp,cpp}`, `analysis.cpp`,
  `export_json.cpp`, `src/mcp/mcp_server.cpp`, `src/cli/main.cpp`, `src/cli/CMakeLists.txt`,
  `src/client/main.cpp`, `src/engine/CMakeLists.txt`, `tests/smoke/report_test.cpp`,
  `tests/smoke/{CMakeLists.txt,pipeline_test.cpp,analysis_test.cpp,export_json_test.cpp}`,
  `docs/host-skill-contract.md`, `integrations/skills/cgraph/SKILL.md`, `README.md`.

## Capabilities

### Modified Capabilities
- `graph-daemon-client` — the `report` op, its modules view, whole-row budgeting, the upgrade hint.
- `host-integration-mcp` — the `graph_report` tool and skill routing.
- `deterministic-graph-pipeline` — `modules.mmd`/`modules.svg` exports; graph.html layout scale
  and community-collapsed first paint.
