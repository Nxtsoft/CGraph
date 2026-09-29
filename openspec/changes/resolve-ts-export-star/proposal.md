# Imports through TypeScript re-export barrels reach the declaration

## Why

TypeScript projects route imports through barrel files. turing-api's services write `import { competitors } from '../../db/schema'`, where `db/schema/index.ts` holds only `export * from './library'` and `library/index.ts` holds only `export * from './competitors'`. CGraph resolves such an import to the barrel *file*, because the barrel declares no `competitors`, and stops there. So `impact` on the Drizzle table object never reaches the services that use it.

The area 7 measurement (`~/.agents/artifacts/2026-09-25/cgraph-context-engine-research/briefs/area7-data-seams/results.md`) found that resolving `export *` barrels, together with a SQL-to-ORM link, lifts table-to-handler coverage at engine defaults from 0 to 0.955, at precision 0.823. This change is the barrel half. It helps every TypeScript `impact` query that crosses a barrel. CGraph already follows re-export chains for Rust `pub use` (issue #60); TypeScript needs the same follow plus star re-exports.

## What Changes

- The JavaScript/TypeScript extractor marks re-exports on the per-file `re_exports` edge: `star=true` for `export * from './x'` (but not `export * as ns`), and `reexport=<exported name>` for `export { a as b } from './x'`. The mark goes on the edge, not the stub, because stubs are shared by every file importing the same path.
- `resolve_imports` builds its re-export maps from those edges, plus the existing Rust node tag, so one stub may be re-exported by several barrels. When an item import still sits on a barrel file after the named chain, a bounded (8 hops), cycle-safe breadth-first search walks the barrel's star targets. It stops at the first file that declares the name exactly once, or that re-exports it by name. A name found nowhere keeps the import on the barrel, as before.

## Contract that tests verify

- `import { competitors } from '../../db/schema'` through two nested `export *` barrels gets an `imports` edge to `competitors` in `competitors.ts`.
- `export { widgets as gadgets } from './widgets'` inside a starred barrel resolves `import { gadgets }` to `widgets`.
- Two barrels that both star the same module each resolve their importers to the declaration.
- A cycle of star barrels terminates, and an import of an undeclared name stays on the barrel file.
- Rust `pub use` follow is unchanged: its existing tests pass as they are.

## Non-goals

- `export * as ns from` (a namespace object, not a merge of names).
- CommonJS `module.exports = require(...)` re-exports.
- Resolving which duplicate wins when two star targets both declare the name: the first found in breadth-first order is taken, and ties at the same depth follow edge order.

## Impact

- `src/engine/javascript_extractor.cpp`: edge marks on `re_exports` edges.
- `src/engine/graph_builder.cpp`: `resolve_imports`' re-export follow is built from edges and extended with star targets.
- `tests/smoke/graph_builder_test.cpp`: `check_typescript_export_star` through `run_one_shot`; `javascript_extractor_test.cpp`: the edge marks.
- `merge_fragments` merges the `star` and `reexport` marks of duplicate edges (a change to the dedup/merge contract), so `kIndexVersionKey` is bumped to `logic-6`.
- `graph.json`: TypeScript `imports` edges that used to stop at barrel files now reach declarations. No node or id changes.
