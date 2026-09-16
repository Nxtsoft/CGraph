# Feature: `report design` — entry points, top call flows and layers (CGR-11)

## Why

"Program designs" was the second of the four asks that started the CGR-7 track, and the plan's
landscape review found it the clearest whitespace: Structurizr, LikeC4 and IcePanel need a
hand-written model, ArchUnit-style tools enforce layers a human named, and layer or entry-point
recovery from a parsed graph exists only in papers. The field notes also flagged the export that
was supposed to serve this need: `call-flow.html` is one `<li>` per `CALLS` edge with full node
ids, no ordering, no entry points, no grouping -- 1,860 lines on CGraph's own tree.

Everything the view needs is in the graph: function nodes with files and lines, `CALLS` and
`dispatches_to` edges, the route-handler nodes CGR-4's follow-up added, and the directory
grouping the modules view already uses.

## What Changes

- **Entry points.** Function nodes in scope (test roots excluded unless `include_tests`),
  classified most-specific first: `main` (`main`/`Main`/`__main__`); `route` (a label of the
  inline handler shape `<x>.<verb> /path`, or a `GET`/`POST`/... export in a Next.js
  `app/**/route.ts`); `page` (an uncalled function in `app/**/page.tsx`, `layout.tsx`,
  `template.tsx`, `loading.tsx`, `error.tsx`, `not-found.tsx` or under `pages/`); `root` (any
  other function with at least one callee that nothing in the graph calls). Ranked by `reach`,
  the number of functions transitively reachable over `CALLS`/`dispatches_to`, then fan-out,
  then label.
- **Flows.** From each entry a tree to `hops` (default 3): children ordered by reach, capped at
  four per node with the rest counted in `more`, each function drawn once per flow so a cycle
  ends where it re-enters. Nodes carry root-relative file, line and reach.
- **Layers.** A multi-source BFS from every entry point over the call adjacency assigns each
  function its shortest call distance; the report lists, per depth, how many functions sit
  there and the three modules holding most of them, plus the count of functions no entry
  reaches (with up to five samples, most called first) -- dead code or unresolved callers.
- **Renderers.** `json` (entry points with nested flows, layers, totals by kind, omitted),
  `markdown` (entry table, nested flows, layer table, unreached line) and `mermaid` (a
  `flowchart TD` of the kept flows; shared callees drawn once; entries styled). `svg` is refused
  with `report_format_unsupported`: the modules SVG is a hand-laid layered DAG, a flowchart is
  not. Budget shedding drops whole entry points from the tail of the ranking.
- **Exports.** One-shot builds write `design.mmd` and `design.md` and no longer write
  `call-flow.html`; `export_call_flow_html` is removed.
- Surfaces: `cgraph report design` (markdown default, `--hops`), MCP `graph_report`
  (`view: "design"`, `hops`, description; no view is reserved any more), host contract, bundled
  skill (an orientation routing row), READMEs (export lists and a section).

### Non-goals
- Entry points from frameworks the extractor does not name (Django URL confs, Spring
  annotations, CLI subcommand tables): they appear as `root` when nothing calls them, or not
  at all when the framework calls them through reflection the graph cannot see.
- A layer *assignment* that says which functions belong to which architectural layer. Call
  distance is what the graph knows; naming layers is the modules view's longest-path ranking.
- Flows across repositories (CGR-13/14).
- A rendered SVG.

## Impact

- **Touches:** `src/engine/include/cgraph/report.hpp`, `src/engine/report.cpp`,
  `src/engine/pipeline.cpp`, `src/engine/include/cgraph/pipeline.hpp`, `src/engine/export_json.cpp`,
  `src/engine/include/cgraph/export_json.hpp`, `src/cli/main.cpp`, `src/mcp/mcp_server.cpp`,
  `tests/smoke/report_test.cpp`, `tests/smoke/pipeline_test.cpp`, `tests/smoke/export_json_test.cpp`,
  docs.
- `graph.json` is unchanged. One export is removed (`call-flow.html`) and two added.
- Measured on CGraph's own tree (733 functions outside test roots), defaults, 70 ms per call:
  60 entry points -- 10 `main` (the CLI `src/cli/main.cpp:887` with reach 483 and fan-out 14
  leads, then `graphd`'s main at 404, then the MCP, client and the two Python scripts) and 50
  `root`; layers 0-10 with `src/engine` holding every layer past 3; 635 functions reached, 38
  unreached (constructors/destructors of RAII scopes such as `EnrichmentRunningScope`,
  `FileWatcher`, `ScopedTimer`, which nothing "calls"). The default budget keeps 24 of 60 entry
  points. `design.md` is 703 lines and `design.mmd` 757 for the whole tree.
- Measured on the `frontend` Next.js app with `scope: "app"`, 0.16 s per call: 389 entry points
  -- 199 `page` components in 124 `page.tsx`/`layout.tsx` files, 18 `route` handlers (`GET`,
  `POST`, `DELETE` exports in `app/**/route.ts`), 172 `root` -- the largest flows being
  `ProjectDetailPage` (17 hooks) and `NextRoundPage` (13). Two honest caveats the numbers carry:
  React components rendered as JSX are not `CALLS` edges, so a component used only in JSX is a
  `root` (if it calls hooks) or `unreached` (707 of 1,387 here); and without `scope` the top of
  the list is minified Playwright trace bundles under `playwright-report/`, which the detector
  should skip (a follow-up, also seen by `report clones`).
- **Found while benchmarking, not caused by this view:** CGraph's own one-shot build went from
  0.86 s to 13-20 s. `stats.json` puts all of it in `communities_ms`, which also covers
  `write_layout`; that routine switches from Fruchterman-Reingold to DrL at exactly 2,000 nodes
  (`analysis.cpp`, `kDrlThreshold`), and adding this view's source pushed the repo from 1,948
  to 2,007-2,022 nodes. Bisected file by file: any single changed file stays under 2,000 and
  builds in ~255 ms; `report.cpp` + `report.hpp` together cross it and take 12.7 s. The design
  report itself costs 70 ms. DrL is 50x slower than FR at this size, the opposite of the
  comment's intent; the threshold needs measuring and moving in its own change.

## Capabilities

### Modified Capabilities
- `graph-daemon-client` — the `report` op serves a `design` view.
- `deterministic-graph-pipeline` — one-shot builds write the design report instead of call-flow.html.
- `host-integration-mcp` — `graph_report` exposes the design view and `hops`.
