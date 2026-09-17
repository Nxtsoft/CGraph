## Why

Kotlin is registered as a supported language (`kotlin_config`, `DetectedLanguage::Kotlin`, a
vendored `tree-sitter-kotlin` grammar, and a golden-test case), but the extractor produces **zero
symbols and zero edges** on real Kotlin source. Root cause: the `fwcd/tree-sitter-kotlin` grammar
exposes **no named fields** on its declarations — `class_declaration`, `object_declaration`, and
`call_expression` have empty field tables, and `function_declaration` carries only `receiver`.
`kotlin_config` relied on `name_fields = {"name"}` / `body_fields = {"body"}` / an accessor field
for the callee, none of which exist in this grammar, so `label_for_node` finds no name and skips
every construct, and `add_raw_call` labels each callee with the whole call's text. A Kotlin repo
therefore extracts an empty graph — the "configured" state was never functional.

## What Changes

- Add `kotlin_symbol_name` (a `resolve_function_name`): resolve a class/object name from the
  `type_identifier` child and a function name from the `simple_identifier` child, positionally,
  since the grammar has no `name` field.
- Add `kotlin_callee_name` (a `resolve_callee_name`): given the field-less `call_expression`,
  descend to the callee's bare leaf name — a `navigation_expression` (`recv.member`) reduces to its
  `navigation_suffix` `simple_identifier`, a `simple_identifier` callee (`f()`, `Widget()`) is taken
  verbatim — kept non-member so it resolves project-wide by name, mirroring how Java's
  `method_invocation` name resolves across files.
- Extend `add_raw_call` (the shared configured walker) so that when a call node exposes **no**
  accessor field at all but the config supplies a `resolve_callee_name`, the whole call node is
  handed to the resolver. This is backward-compatible: every existing configured language declares a
  callee field, so only the field-less Kotlin `call_expression` takes the new path.
- Drop the ineffective `interface_declaration` node type (an interface is a `class_declaration` with
  an `interface` modifier in this grammar) and the unused `name_fields`/`body_fields`/
  `call_accessor_fields` from `kotlin_config`.

### v1 non-goals (recorded follow-ups)
- **Imports.** Kotlin `import_header` paths are package-qualified and decoupled from file layout
  (like Rust `use`), so `resolve_imports` would drop every stub. No import handler is wired.
- **Interface dispatch.** `implements`/`dispatches_to` for Kotlin interfaces (the Go/`#47` mechanism)
  is deferred; direct and project-wide-name call resolution already connect tests to code.

## Impact

- Kotlin source now produces class/object/function nodes and `CALLS` edges with the same
  Graphify-compatible fragment shape as other configured languages. Verified end-to-end: a real
  Kotlin project that previously extracted **2 files / 2 nodes / 0 edges** now extracts **13 nodes /
  16 edges**, with constructor-style calls (`Calc()`) and navigation calls (`c.add()`) resolving.
- **Touches:** `src/engine/configured_extractors.cpp` (two resolvers + `kotlin_config`),
  `src/engine/extractor.cpp` (the no-accessor-field callee path),
  `tests/smoke/configured_extractors_test.cpp` (`check_kotlin_extraction`). No new translation unit.

## Capabilities

### Modified Capabilities

- `deterministic-graph-pipeline` — Kotlin source extraction (repair).
