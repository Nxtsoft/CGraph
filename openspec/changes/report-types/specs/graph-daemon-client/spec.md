## ADDED Requirements

### Requirement: Report op serves a types view

The `report` op with `view: "types"` SHALL audit type definitions. A type is a `class` or `type` node with a source file, outside test roots unless `include_tests`, and under `scope` when given; its file SHALL be reported root-relative; its members SHALL be the labels of the `field` nodes it `defines`, and its uses SHALL be the incoming edges whose relation is not `contains`, `defines`, `method` or `method_of` and whose source is not one of its own fields. Only types with at least `min_members` members (default 3) take part in shape comparison. The response SHALL carry four sections of whole rows: `identical` (groups of differently named types whose member sets are equal, each group with its `shape` and its `types`; widest shape first, then largest group), `duplicates` (a label declared in two or more files, each declaration with file, line and members, plus the lowest and highest pairwise member-set Jaccard among the declarations; highest `min_jaccard` first, then most declarations), `overlaps` (pairs of differently named types classified `subset` when one member set is contained in the other and the smaller is at least half the size of the larger, with the smaller type first, or `overlap` when the Jaccard is at least `threshold`, default 0.80; Jaccard descending, then shared descending), and `unreferenced` (types with no use, most members first). The response SHALL carry `totals {types, with_members, identical, duplicates, overlaps, unreferenced}` and `omitted` with the same four section keys. When the rendered report exceeds `budget`, rows SHALL be shed whole from the tail of the last populated section in the order unreferenced, overlaps, duplicates, identical, and `omitted` SHALL count them. The view SHALL render `json` and `markdown` (which lists at most eight declarations per duplicate row before "+N more files"); `mermaid` and `svg` SHALL be refused with `ok: false` and `code: "report_format_unsupported"`. A `threshold` outside [0, 1] or a `min_members` below 1 SHALL be refused as a parameter error.

#### Scenario: A type name declared in two headers is a duplicate row
- **GIVEN** `FileState {size, modified_at, kind, token}` in one file and `FileState {size, modified_at, drop}` in another
- **WHEN** `report` is called with `view: "types"`
- **THEN** `duplicates` holds one row labelled `FileState` with two declarations, each with its root-relative file and line, and `min_jaccard` and `max_jaccard` are 0.40

#### Scenario: Equal member sets form one identical group
- **GIVEN** `Point {x, y, z}` and `Vec3 {z, y, x}`
- **THEN** `identical` holds one group with shape `x, y, z` and both types, and no `overlaps` row pairs them

#### Scenario: A nested shape is a subset only when at least half
- **GIVEN** `Base {id, name, createdAt}` with `Extended {id, name, createdAt, deletedAt}`, and `Tiny {id}` inside `Base` at `min_members` 1
- **THEN** `overlaps` holds `Base`/`Extended` as `subset` with `Base` first and Jaccard 0.75, and no row pairs `Tiny` with `Base`

#### Scenario: A near miss and a tiny pair are not rows at the defaults
- **GIVEN** `Near {a, b, c, d, e}` with `Nearby {a, b, c, d, f}` (Jaccard 0.67) and `Tiny {id}` with `Tiny2 {id}`
- **THEN** neither pair appears at `threshold` 0.80 and `min_members` 3
- **AND** at `threshold` 0.6 the first pair appears as `overlap`, and at `min_members` 1 the second is an identical group

#### Scenario: Structural edges do not make a type referenced
- **GIVEN** a type whose only incoming edges are `contains` from its file and `references` from its own field
- **THEN** it appears in `unreferenced`
- **AND** a type that is referenced by a function, constructed through a `CALLS` edge, or inherited from does not

#### Scenario: The budget sheds unreferenced rows first and keeps identical groups last
- **WHEN** the budget is one token below the full report
- **THEN** the report loses rows from the end of `unreferenced` only and `omitted.unreferenced` equals the number dropped
- **AND** under a budget that fits only one row, that row is the first identical group

#### Scenario: Diagram formats are refused
- **WHEN** `report` is called with `view: "types"` and `format: "mermaid"` or `"svg"`
- **THEN** the response is `ok: false` with `code: "report_format_unsupported"`
