## Why

cgraph extracts 12 languages (C, C++, C#, Go, Groovy, Java, JS, TS, Kotlin, Python, Ruby, Scala)
but not Rust — a major language for the coding agents cgraph serves. Rust fits the existing
grammar-driven configured-extractor path (no hand-written C++), so it is a well-scoped add.

## What Changes

- Add the `tree-sitter-rust` grammar (pinned v0.24.2, LANGUAGE_VERSION 15 — inside the vendored
  core's ABI window [13,15]) as a vendored submodule + build wiring (`parser.c` AND the external
  `scanner.c`).
- Register `DetectedLanguage::Rust`, map `.rs`, and declare `rust_config()` in
  `configured_extractors.cpp`: functions (`function_item`, `function_signature_item`), types
  (`struct_item`/`enum_item`/`union_item`/`trait_item`/`type_item`), calls (`call_expression`),
  and `x.method()` member calls (`field_expression`.`field`).
- Add `rust_callee_name`: Rust's `Type::method()` (`scoped_identifier`) and turbofish
  `foo::<T>()` (`generic_function`) are shapes `cpp_callee_name` does not descend, so reusing it
  would silently drop those calls. The Rust resolver descends `scoped_identifier.name`,
  `generic_function.function`, and `field_expression.field` to the leaf identifier.

### v1 non-goals (each a recorded follow-up)
- **`use` imports.** `resolve_imports` matches `/`-delimited path-like specs by file suffix and
  DELETES any stub resolving to no project file. Rust `use` paths are `::`-delimited and decoupled
  from file layout (`mod.rs`, re-exports), so every emitted stub would be dropped — zero surviving
  edges. A real Rust module resolver is the #1 follow-up.
- **impl→type method attribution.** impl blocks carry no name; methods inside are captured as
  file-contained functions (exactly like Go's methods). Linking them to their `impl` type needs a
  relation_handler — deferred.
- **Macros** (`macro_invocation`), **modules** (`mod_item` as a node — the namespace "god-node"
  precedent), and closures as call scopes — all deferred.

## Impact

- New language coverage: `.rs` files now produce function/type/call nodes and edges with the same
  fragment shape as other configured languages. Verified end-to-end: a real Rust project's
  `struct`, free/impl functions, `contains`, and `CALLS` edges (incl. a `self.tick()` member call
  resolving same-file) all appear in `graph.json`.
- **Touches:** `.gitmodules` + `vendor/tree-sitter/grammars/rust` submodule,
  `vendor/tree-sitter/CMakeLists.txt`, `src/engine/detect.{hpp,cpp}`,
  `src/engine/configured_extractors.cpp`, `tests/smoke/configured_extractors_test.cpp`,
  `tests/smoke/extractor_goldens_test.cpp`. No new engine translation unit (config is inline like
  `go_config`), so no `src/engine/CMakeLists.txt` change.

## Capabilities

### Modified Capabilities

- `deterministic-graph-pipeline` — Rust source extraction.
