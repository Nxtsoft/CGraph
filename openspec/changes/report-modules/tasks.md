# Tasks

## 1. Test first
- [x] 1.1 `tests/smoke/report_test.cpp` (1:1 with `report.cpp`): grouping by depth with test roots
      excluded, `include_tests`, longest-path layers, cycle listing with dashed edges, scope and
      depth semantics, budget shedding (edges first, then modules, `omitted` reported, no partial
      rows, budget 0 = unlimited), the four renderers, determinism, the daemon envelope (reserved
      views, unknown view, mistyped and out-of-range params), and the upgrade hint for a graphd
      that answers `unknown op: report`.
- [x] 1.2 Register the target in `tests/smoke/CMakeLists.txt`; extend `pipeline_test.cpp` to expect
      `modules.mmd` and `modules.svg` from `write_exports`.

## 2. The report op
- [x] 2.1 `DaemonOp::Report` appended before `Count` (`operation_stats.hpp`) and named `report` in
      the op-name table; the durable ledger's `kSubstantiveOps` is unchanged.
- [x] 2.2 `report.hpp` / `report.cpp`: `ReportRequest`, `parse_report_request`,
      `build_modules_report`, `shed_to_budget`, the JSON/Mermaid/SVG/Markdown renderers,
      `report_response`, `report_upgrade_hint`.
- [x] 2.3 `DaemonState::project_root`, set by both daemon servers, so module names are
      root-relative; `handle_daemon_request` dispatches `Report` as a root-pinnable read.

## 3. Surfaces
- [x] 3.1 MCP tool `graph_report` (`view` enum modules|design|clones|types, `format`, `scope`,
      `depth`, `include_tests`, `budget`, `expected_content_root`); an old daemon's `unknown op`
      becomes the upgrade hint.
- [x] 3.2 CLI `cgraph report <view> [--root] [--format] [--scope] [--depth] [--budget]
      [--include-tests] [--daemon]` over the thin-client runtime (spawns graphd); diagram on
      stdout, `omitted` summary on stderr, exit 3 with the upgrade hint against an old daemon.
- [x] 3.3 `cgraph-client` usage lists `report` (it already forwards any op).
- [x] 3.4 `write_exports` takes the project root and writes `modules.mmd` + `modules.svg`.

## 4. graph.html
- [x] 4.1 `analysis.cpp::write_layout` rescales igraph coordinates to a canvas-sized square.
- [x] 4.2 The viewer starts community-collapsed above 500 nodes; click or search expands.

## 5. Measure
- [ ] 5.1 `cgraph report modules --root . --depth 2 --format mermaid` on CGraph at origin/main
      (scope src, then `--include-tests`); quote counts in the proposal and PR.
- [ ] 5.2 Render `modules.svg` for CGraph, turing-webapp, and a Python corpus; screenshots on the PR.
- [ ] 5.3 graph.html before/after first-paint screenshots on the CGraph graph.
- [ ] 5.4 Full suite green (`ctest --preset default`); `cgraph_file_watcher_test` is a known
      tmpfs failure on this host.

## 6. Docs
- [x] 6.1 `docs/host-skill-contract.md` lists `report`; the `cgraph` host skill routes
      architecture / module-map questions to `graph_report`; README gets a report section.
