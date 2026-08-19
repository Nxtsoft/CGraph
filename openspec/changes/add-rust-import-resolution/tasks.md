# Tasks

## 1. Failing tests first

- [x] 1.1 `tests/smoke/configured_extractors_test.cpp`: `check_rust_use_imports` asserts
      the stub shape (kind/label/`import_path`/`module_layout`) for plain scoped, aliased
      (`as` — original name, alias never a node), grouped `{a, b::c}`, glob `*`, `{self}`,
      external-crate, and `super::` forms, parsed from real Rust source, plus the
      file → stub `imports` edges.
- [x] 1.2 `tests/smoke/graph_builder_test.cpp`: `test_resolve_rust_imports` covers item →
      declared symbol, module import → module file, glob → module file, `mod.rs` layout,
      undeclared item → file node, external crate dropped with its edge, and ambiguous
      suffix dropped.

## 2. Implement

- [x] 2.1 `src/engine/configured_extractors.cpp`: `rust_import_handler` (use-tree walker)
      + `import_node_types = {"use_declaration"}` registration, replacing the v1 non-goal
      comment.
- [x] 2.2 `src/engine/graph_builder.cpp`: `rust_module_match` (`<path>.rs` / `<path>/mod.rs`,
      unique or nothing) and the full-path-then-parent retry for `module_layout=rust` stubs.

## 3. Golden coverage

- [x] 3.1 `tests/smoke/extractor_goldens_test.cpp`: Rust `use` case asserting the stub label
      survives extraction. Every golden diff enumerated: one new Rust case added, no
      existing case changed.

## 4. Prove it end-to-end

- [x] 4.1 Run the real `cgraph` binary over a Rust fixture project exercising both layouts
      (`bar.rs` and `qux/mod.rs`) plus an external-crate `use`; quote the resulting
      `imports` edges from `graph.json`, including the external-crate negative case.
      Measured (4 files, 8 nodes, 9 edges, 8 ms): `main.rs -> foo/bar.rs:Baz` (item, .rs
      layout), `main.rs -> qux/mod.rs:run_q` (item, mod.rs layout), `main.rs -> foo/bar.rs`
      (module import -> file node); zero serde nodes/edges; zero surviving import/module
      stubs.
- [x] 4.2 `ctest --preset default` full suite green (66/66, 39s). Sanitizers via CI, the
      same call as add-rust-extractor: locally the ASan-built binaries hang at startup
      with no output (three `ctest --preset sanitizers` attempts stalled at the first,
      untouched test; a direct binary run produced zero output) -- an environment issue,
      not this change. The PR's macos/ubuntu sanitizers jobs are the gate.
