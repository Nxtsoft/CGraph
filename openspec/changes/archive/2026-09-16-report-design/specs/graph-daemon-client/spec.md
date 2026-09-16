## ADDED Requirements

### Requirement: Report op serves a design view

The `report` op with `view: "design"` SHALL list the program's entry points and the top call flow from each. Candidates SHALL be `function` nodes with a source file under `scope`, outside test roots unless `include_tests`. An entry point SHALL be classified, most specific first, as `main` (label `main`, `Main` or `__main__`), `route` (a label of the form `<receiver>.<verb> /path` or `<verb> /path` with an HTTP verb, or a function named for an HTTP verb in an `app/**/route.*` file), `page` (a function with no callers in an `app/**/{page,layout,template,loading,error,not-found}.*` file or under `pages/` outside `api/`), or `root` (any other function with at least one callee over `CALLS` or `dispatches_to` and no caller). Entry points SHALL be ranked by `reach` (distinct functions transitively reachable over those relations) descending, then fan-out descending, then label. Each entry SHALL carry a `flow`: a tree to `hops` (default 3, minimum 1) whose children are ordered by reach and capped at four per node with the remainder in `more`, each function appearing at most once per flow. The response SHALL carry `layers` (for each shortest call distance from any entry point, the function count and up to three module names holding most of them), `unreached_samples` (up to five labels of functions no entry reaches, most called first), `totals {functions, entry_points, by_kind, reached, unreached}` and `omitted {entry_points}`. When the rendered report exceeds `budget`, whole entry points SHALL be shed from the tail of the ranking and `omitted` SHALL count them. The view SHALL render `json`, `markdown` and `mermaid` (a `flowchart TD` in which a callee shared by several flows is one node); `svg` SHALL be refused with `ok: false` and `code: "report_format_unsupported"`. A `hops` below 1 SHALL be refused as a parameter error.

#### Scenario: Entry points of four kinds ranked by reach
- **GIVEN** `main` calling seven functions with a chain three deep, a `app.get /health` handler, a component in `app/dashboard/page.tsx`, and an uncalled `exported` with one callee
- **WHEN** `report` is called with `view: "design"`
- **THEN** `entry_points` lists `main` (`main`, reach 10), the page component (`page`), the handler (`route`) and `exported` (`root`), in that order

#### Scenario: Called, leaf and cyclic functions are not entry points
- **GIVEN** a function with no callees, two functions that call each other, and a function called by `main`
- **THEN** none of them is an entry point and the cycle members count as unreached

#### Scenario: A flow is bounded by hops and branch
- **GIVEN** `main` with seven callees
- **THEN** its `flow` draws the four callees with the largest reach, reports `more: 3`, and follows the deepest chain three hops; with `hops: 1` the children have no children while `reach` is unchanged

#### Scenario: Layers are shortest call distances
- **GIVEN** a function reachable at distance 2 from one entry and 3 from another
- **THEN** it is counted once, at depth 2

#### Scenario: The budget sheds the last entry point
- **WHEN** the budget is one token below the full report
- **THEN** the lowest-ranked entry point is omitted and `omitted.entry_points` is 1 while `layers` are kept
