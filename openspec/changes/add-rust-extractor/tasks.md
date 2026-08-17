# Tasks

## 1. Grammar
- [x] 1.1 Add `tree-sitter-rust` submodule at v0.24.2 (verify parser.c LANGUAGE_VERSION in [13,15]
      against vendored core 0.25.10 — it is 15).
- [x] 1.2 Wire `grammars/rust/src/parser.c` + `grammars/rust/src/scanner.c` + include dir into
      `vendor/tree-sitter/CMakeLists.txt`.

## 2. Register + configure
- [x] 2.1 `detect.hpp`: add `Rust` to `DetectedLanguage`; `detect.cpp`: `.rs` -> Rust +
      language_name "rust".
- [x] 2.2 `configured_extractors.cpp`: `extern tree_sitter_rust`, `rust_callee_name`,
      `rust_config()`, and both switch cases.

## 3. Prove it
- [x] 3.1 `check_rust_extraction()` in `configured_extractors_test.cpp`: struct/enum/trait/alias
      type nodes; free/impl/trait functions; a plain call, a `x.method()` member call, and a
      `Type::method()` scoped call reduced to its bare name (proves rust_callee_name). Add Rust to
      the config-validation list.
- [x] 3.2 Golden case in `extractor_goldens_test.cpp`.
- [x] 3.3 End-to-end: `cgraph --root <rust-proj>` produces Service (type), run/tick/helper
      (functions), contains + CALLS edges incl. the same-file member call.
- [x] 3.4 Full `ctest --preset default` green (66/66); sanitizers via CI.
