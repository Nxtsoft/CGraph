## ADDED Requirements

### Requirement: Report op with a modules view
The daemon SHALL serve a `report` op selected by `view` (`modules` | `design` | `clones` | `types`). Only `modules` SHALL be implemented by this change; the reserved views SHALL answer `ok: false` with `code: "report_view_not_implemented"` so a host can distinguish "not yet" from a malformed request. An unknown view or format, a `depth` below 1, or a negative `budget` SHALL be an error frame, and a mistyped parameter SHALL yield `invalid request parameter` like every other op. `report` SHALL be a root-pinnable read (`expected_content_root` applies) and SHALL be recorded in op-stats without changing the durable ledger schema.

The modules view SHALL group every code node (enrichment and memory nodes excluded) by the first `depth` directory components of its source file relative to the daemon's project root (default depth 2, so `src/engine/dedup.cpp` belongs to `src/engine`; a root-level file belongs to `.`). It SHALL aggregate `imports`, `imports_from`, `re_exports` (counted as imports) and `CALLS` edges between distinct modules with counts, rank layers by the longest dependency path over the module DAG (layer 0 = nothing depends on it; a strongly connected component shares one layer), and list every cycle as the sorted members of a strongly connected component. Modules under a test root (`test`, `tests`, `testing`, `__tests__`, `spec`, `specs`, `e2e`, `testdata`, `fixtures`, `__mocks__`) SHALL be left out entirely -- neither as sources nor as targets, since an edge from production code into a test is a name-collision resolution artifact -- unless `include_tests` is true. `scope` (a root-relative prefix) SHALL restrict which modules report their outgoing edges; a module they depend on SHALL still appear as a target.

#### Scenario: Files group into modules by depth
- **GIVEN** files `a/x/f1.py`, `a/x/f2.py`, `a/y/g.py`, `b/h.py` with calls a/x -> a/y (2), one import a/x -> a/y, and b -> a/x (1 call, 1 import)
- **WHEN** `report {view: "modules"}` runs at depth 2
- **THEN** modules are `b` (layer 0), `a/x` (layer 1, 2 files), `a/y` (layer 2), with edges `a/x -> a/y` (2 calls, 1 import) and `b -> a/x` (1 call, 1 import), and `cycles` is empty

#### Scenario: Test roots are excluded by default
- **GIVEN** `tests/t.py` calls into `a/x` five times
- **WHEN** the report runs without `include_tests`
- **THEN** `tests` is not a module and no edge leaves or enters it
- **AND** with `include_tests: true` the edge `tests -> a/x` (5 calls, 1 import) appears and `tests` sits in layer 0

#### Scenario: A cycle is listed, not hidden
- **GIVEN** `a/y` also calls `a/x`
- **WHEN** the report runs
- **THEN** `cycles` contains `["a/x", "a/y"]`, both edges carry `cycle: true`, and both modules share one layer

#### Scenario: Scope reports dependencies of the selected modules
- **WHEN** the report runs with `scope: "b"`
- **THEN** `b -> a/x` is reported and `a/x` appears as a target, while `a/x -> a/y` is not reported

#### Scenario: A reserved view is a typed error
- **WHEN** `report {view: "design"}` runs
- **THEN** the response is `ok: false` with `code: "report_view_not_implemented"`

### Requirement: Report output is budgeted by whole rows
The report SHALL render `json`, `mermaid` (`graph LR`, one subgraph per layer, module labels carrying file counts, edge labels carrying call and import counts, cycle edges dashed), `markdown` and `svg` (the daemon's own layered layout, no Graphviz). The rendered text SHALL be measured at about four characters per token against `budget` (default 6000; 0 disables the budget). When it overflows, the daemon SHALL drop whole dependency rows lightest first, and only when the bare module list still overflows drop whole modules lightest first (an edge leaves with either endpoint). A row SHALL never be truncated. The response SHALL always carry `totals` and `omitted` counts for modules and edges, plus `estimated_tokens` and `budget`.

#### Scenario: Edges are shed before modules
- **GIVEN** a 30-module chain whose edge weights strictly increase
- **WHEN** the budget is below the full report but above the module list
- **THEN** `omitted.edges > 0`, `omitted.modules == 0`, the kept edges are the heaviest, and the rendered text fits the budget

#### Scenario: Modules are shed when the list itself overflows
- **WHEN** the budget cannot hold the module list
- **THEN** the lightest modules are dropped, `omitted.modules > 0`, every kept edge joins two kept modules, and the rendered text says what was omitted

#### Scenario: Budget zero renders everything
- **WHEN** `budget` is 0
- **THEN** nothing is omitted

### Requirement: An older daemon surfaces an upgrade hint
A graphd built before the op answers `unknown op: report`. The CLI (`cgraph report`) and the MCP tool SHALL turn that reply into an "upgrade the daemon" message that names the shutdown command, never an empty report; the CLI exits 3 in that case.

#### Scenario: Old daemon
- **WHEN** the daemon replies `{ok: false, error: "unknown op: report"}`
- **THEN** the client prints the upgrade hint instead of a diagram
