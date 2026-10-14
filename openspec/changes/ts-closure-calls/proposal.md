# TypeScript call edges from closures and test callbacks (CGR-26)

## Why

The 2026-10-13 head-to-head found 7 of 11 caller files of `assertProductCategoryAccess` on turing-api (codebase-memory-mcp found 11). All four misses were calls inside closures: Effect `try:` bodies written into a module-level service object (`outcome-merge/service.ts`, `outcomes/service.ts`) and `it` callbacks in two integration tests. CGraph had the import edges from all four files and no CALLS edge.

The walker treated every JS/TS arrow that is not a module-level `const f = () => {}` (or an HTTP route's inline handler) as a boundary and dropped the calls in its body, Graphify's function-body seeding. A closure's calls are calls its enclosing code makes, so "who calls X" missed every caller that reaches X through a callback.

## What Changes

- A boundary arrow still gets no node and is still no function scope (contract readers, env reads and HTTP consumers see what they saw before), but its calls keep a caller: the enclosing function or method. With none, the new `LanguageConfig::closure_call_scope` hook names it; for JS/TS that is the class for a class-field arrow, else the module-level binding the closure is written into when it has a node (`ServiceLive` in `export const ServiceLive = Layer.succeed(Tag, {...})`), else the file (a test file's `describe` / `it` callbacks). A language without the hook drops them as before.
- Such calls carry `RawCall::in_closure`. Closures are where parameters, destructured props and hook setters are called, so `resolve_raw_calls` binds them on evidence only (same file, an import, a receiver naming its class) and refuses the bare-name tiers (a unique project label with no import, a unique method for an unknown receiver), counted as `dropped_closure_guess` in `stats.json`. Calls outside closures resolve exactly as before.
- Index version `logic-23`.

## Impact

- `src/engine/extractor.cpp` (call scope threaded through `walk_node`), `src/engine/include/cgraph/language_config.hpp` (`ClosureCallScope`, `RawCall::in_closure`), `src/engine/javascript_extractor.cpp` (the JS/TS hook), `src/engine/graph_builder.cpp` (evidence-only binding), `src/engine/operation_stats.cpp` / `operation_stats.hpp` (`dropped_closure_guess`), `src/engine/index_persistence.cpp` (`logic-23`).
- Tests: `extractor_test.cpp`, `javascript_extractor_test.cpp` (with fixture `tests/fixtures/ts_closure_calls/`), `graph_builder_test.cpp`, `index_persistence_test.cpp`.
- Graph output: CALLS edges only. No node, no other relation and no node id changes on turing-api, turing-webapp or CGraph. Graphify parity of shape and ids is unaffected; the CALLS set is a deliberate superset of Graphify's, as with route handlers.
