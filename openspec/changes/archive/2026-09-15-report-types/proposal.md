# Feature: `report types` — duplicate, overlapping and unused type definitions (CGR-9 part 2)

## Why

A friend's ask that started the CGR-7 track: "identify custom type definitions bloat". No shipped
tool in any language flags two differently named types with the same shape (knip, staticcheck
U1000 and clang-tidy detect unused only; similarity-ts has a disabled experimental type mode;
golang/go#64945 is an open request). CGraph's own tree shows the three shapes the report must
catch: `FileState` declared in both `file_watcher.hpp:48` and `semantic_drop.hpp:35` with `size`
and `modified_at` in common; test helpers and models whose member sets coincide under different
names; and types nothing refers to.

The inputs landed in two steps. PR #77 added the `report` op with a `modules` view and reserved
`types`. PR #79 (CGR-9 part 1) gave TypeScript, Go, Rust, Python and Java `field` nodes with
`defines` edges, as C/C++ already had, and stopped forward declarations minting class nodes. This
change is the view over them.

## What Changes

- `ReportView::Types` is implemented. `build_types_report` takes every `class`/`type` node with a
  source file outside test roots (unless `include_tests`) and under `scope`, reads its members as
  the labels of the `field` nodes it `defines`, and counts non-structural incoming edges (anything
  but `contains`/`defines`/`method`/`method_of`, excluding edges from the type's own fields).
- Four sections, each a whole row, in the order the budget keeps them:
  - `identical`: groups (union-find over pairs) of differently named types with at least
    `min_members` (default 3) members whose member sets are equal, each with the shared `shape`.
    Widest shape first. Grouping matters: on the frontend repo 115 identical pairs are 45 groups,
    two of them seven types wide (`*ListResponse`, `*CalculationReference`).
  - `duplicates`: one label declared in two or more files, with every declaration's file:line and
    members and the lowest/highest pairwise member-set Jaccard, so a copy (1.0) reads differently
    from an unrelated homonym (0.0). Highest `min_jaccard` first, so `Props` declared in 26
    component files sinks below `ProjectData` copied four times.
  - `overlaps`: pairs classified `subset` (one set inside the other and the smaller at least half
    the larger, smaller type first) or `overlap` (Jaccard at or above `threshold`, default 0.80).
    Bare containment produced 923 subset rows on the frontend repo (every `{id, name, x}` inside
    every record); the half rule leaves 457. Candidate pairs come from an inverted index on member
    label, so unrelated types are never compared. Jaccard desc, shared desc.
  - `unreferenced`: types with no non-structural incoming edge. Most members first. Named for
    what the graph knows: extractors resolve type references across files (imports/includes),
    not within the declaring file, so a component-local type used only there lands here.
- `shed_to_budget(TypesReport&)`: one ranking, identical before duplicates before overlaps
  before unreferenced, each section best-first; a prefix is kept by binary search and `omitted`
  per section is always reported. Budget 0 keeps everything. Markdown caps a duplicate row at
  eight declarations ("+N more files"); JSON keeps all.
- Renderers: `json` (sections plus `totals {types, with_members, identical, duplicates, overlaps,
  unreferenced}` and `omitted`) and `markdown` (four tables and the omitted caption). `mermaid`
  and `svg` are diagram formats for the modules view; `report_response` refuses them for `types`
  with `code: "report_format_unsupported"`. `design` and `clones` remain
  `report_view_not_implemented`.
- `ReportRequest.min_members` (`min_members` param, `--min-members`), `threshold` validated to
  [0, 1]. CLI `cgraph report types` defaults to markdown and prints each view's own totals on
  stderr. MCP `graph_report` documents the view and forwards `threshold` and `min_members`.
- Docs: host contract, bundled skill (a new routing row for type bloat questions), README.

### Non-goals
- Methods are not members. An interface made only of method signatures has an empty member set
  and takes part in `duplicates` and `unreferenced` but not `identical` or `overlaps`.
- Declared type text is not compared: `{id: string}` and `{id: number}` are the same shape here.
- Inherited members are not expanded; `Extended extends Base` with one extra field is a `subset`
  row, which is the finding a reader wants.
- Same-file use is not in the graph (`references` resolve through imports/includes only), so
  `unreferenced` is a lead, not a verdict; the section is named accordingly and the skill text
  says so. The report never removes anything.
- One-shot exports do not write a types file; the view is served by the daemon.

## Impact

- **Touches:** `src/engine/include/cgraph/report.hpp`, `src/engine/report.cpp`,
  `src/cli/main.cpp`, `src/mcp/mcp_server.cpp`, `tests/smoke/report_test.cpp`,
  `docs/host-skill-contract.md`, `integrations/skills/cgraph/SKILL.md`, `README.md`,
  `README.zh-CN.md`.
- No graph or export change. The `report` op gains a view; the CLI's stderr summary line now
  prints whichever totals the view carries instead of `N modules, M edges`.
- Measured on CGraph's own tree (190 files, 1,877 nodes) through the built daemon, default
  parameters: 146 types, 120 with members; 0 identical groups; 2 duplicates -- `PendingEvent`
  (`file_watcher.hpp:60` vs `semantic_drop.hpp:46`, member Jaccard 1.00, a copy) ranks first,
  then `FileState` (`file_watcher.hpp:48` vs `semantic_drop.hpp:35`, 0.40); 0 overlaps; 56
  unreferenced; `FileCacheEntry` is not flagged. Under a second per call; the default-budget
  markdown is ~1.2k tokens.
- Measured on the `frontend` Next.js app (1,261 TypeScript files, 17,439 nodes), default
  parameters, 0.41 s per call: 1,797 types, 1,416 with members; **45 identical groups** (34
  pairs, 4 triples, 5 of four, 2 of seven: the `*ListResponse` and `*CalculationReference`
  families), the widest `BatchFormula`/`FormulaData` at 17 shared members, then
  `InputListQuery`/`UseInputsOptions` (11) and `ReviewComment`/`SetupReviewComment` (9); 94
  duplicates, copies first (`ConnectorTool` and `PageProps` in three files each at 1.00), the
  26-file `Props` homonym at the bottom; 464 overlaps (457 subset, 7 overlap) after the
  half-or-more rule, down from 1,045; 1,180 unreferenced. The default 6,000-token markdown keeps
  all 45 identical groups and 40 duplicate rows and reports the rest as omitted; before the
  value ordering it kept 73 duplicate rows and no identical shapes.
- Known limitation, stated in the skill text and measured here: `unused` lists 56 of the 145
  C++ types, including `Node`, `Edge` and `RawCall`, because the C++ extractor's `references`
  edges resolve only through a file's direct `#include`s (`cpp_extractor.cpp`,
  `allow_same_file = false`), so a type reached through a transitive include has no incoming
  edge at all -- `Node` has exactly one, `contains` from its header. A word-grep finds 25 of the
  56 named in another source file. Widening C++ type-reference resolution to transitive includes
  is a separate precision change; this view reports what the graph knows and says so.

## Capabilities

### Modified Capabilities
- `graph-daemon-client` — the `report` op serves a `types` view.
- `host-integration-mcp` — `graph_report` exposes and forwards the types view's parameters.
