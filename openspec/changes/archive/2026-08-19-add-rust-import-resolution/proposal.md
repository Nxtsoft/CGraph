## Why

Rust extraction (add-rust-extractor, PR #30) deliberately shipped with zero import edges:
`use` paths are `::`-delimited and decoupled from file layout (`a/b.rs` vs `a/b/mod.rs`,
re-exports), so every emitted stub would have resolved to no project file and been dropped.
That change's own record designates this work: "A real Rust module resolver is the #1
follow-up." Until it lands, every Rust project graphed by cgraph has no file-dependency
structure — `use`-based cross-file relationships are invisible to `graph_context` /
`graph_impact`, and cross-file call resolution leans solely on bare-name matching.

The user-visible contract tests verify: a `use` declaration produces an `imports` edge from
the importing file to the module file or the declared item it names, resolved through Rust's
real module layout, and never a guessed or dangling edge.

## What Changes

- `rust_config()` registers `import_node_types = {"use_declaration"}` and a new
  `rust_import_handler` that walks the use-tree grammar shapes (`scoped_identifier`,
  `use_list`, `scoped_use_list`, `use_as_clause`, `use_wildcard`, `self`/`super`/`crate`
  leaves). Each leaf becomes one stub: kind `import`, label = the original (pre-alias) leaf
  name, `properties["import_path"]` = the full `/`-joined path with leading
  `crate::`/`self::`/`super::` stripped, plus a `module_layout=rust` marker. Glob and
  `{self}` leaves name the module itself and are emitted as `module` stubs. Non-plain paths
  (`<T as Trait>::x`, metavariables) are dropped rather than guessed.
- `resolve_imports` gains a layout-aware step for `module_layout=rust` stubs: the path
  resolves as a module file (`<path>.rs` or `<path>/mod.rs`, unique suffix across the
  project or nothing); an unresolved full path on an `import` stub retries its parent as the
  module with the leaf as an item declared in it (falling back to the module's file node
  when the item is not declared). Stub-drop semantics are unchanged: external crates
  (`use serde::…`, `use std::…`) and ambiguous suffixes leave no node and no edge.
- The marker property never reaches an export: every stub carrying `import_path` is consumed
  by `resolve_imports` (remapped or dropped), so graph.json parity is unaffected.
- Non-goals (carried forward unchanged from add-rust-extractor): impl→type method
  attribution, macro expansion, `mod_item` nodes, re-export *chain* resolution (a
  `pub use` parses as a plain `use_declaration` and resolves one hop, which lands "for
  free"; following the chain does not).

## Impact

- Modified capability: `deterministic-graph-pipeline` (the Rust extraction requirement
  gains import resolution).
- Touches: `src/engine/configured_extractors.cpp`, `src/engine/graph_builder.cpp`,
  `tests/smoke/configured_extractors_test.cpp`, `tests/smoke/graph_builder_test.cpp`,
  `tests/smoke/extractor_goldens_test.cpp`. No new translation unit, no CMake change.
- Graphify parity: unaffected — Graphify has no Rust; the fragment/graph.json shape is
  unchanged, only new nodes/edges of already-existing kinds appear, and only for `.rs`
  projects.
