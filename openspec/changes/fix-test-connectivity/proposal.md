# Fix: connect tests to the code they exercise (issues #44, #45)

## Why

Two extraction gaps left test files (near-)disconnected from implementation code, measured as
the fraction of non-test symbols forward-reachable from a repo's tests:

- **#44 (Go, and every member call):** `obj.method()` resolved same-file only, by design. On
  gorilla/mux, route.go's 53 symbols had zero cross-file incoming CALLS — tests reached **11%**
  of the codebase (healthy TS graphs measure 0.52-1.00).
- **#45 (Python):** the import handler emitted a dead-end node (whole statement as label, no
  `import_path`, no edges), so Python graphs carried **no import relations at all**; and
  `obj.method()` calls were unresolvable (no member-call config). On pallets/itsdangerous,
  tests reached **7%** of the codebase.

## What Changes

1. **Method-aware member-call resolution** (`resolve_raw_calls` tier 2b): a member call that
   misses its own file now resolves project-wide when its bare name uniquely names a **method**
   — never a free function, never an ambiguous name — and is always graded INFERRED (no import
   evidence can exist for a bare property name). Methods are recognized by the `method`
   containment relation (class-contained functions) or a new grammar-level tag.
2. **`LanguageConfig.method_node_types`** + extractor tagging (`properties["method"]="true"`):
   Go declares `method_declaration`, covering receiver methods that have no enclosing class
   node.
3. **Python member calls**: `call_member_node_types = {"attribute"}` — `s.sign(...)` is now a
   member call that tier 2b can bind.
4. **Python import stubs rewritten** to the standard module/import stub shape (namespaced ids
   per #42, `import_path` resolve_imports can collapse): dotted specs become path-like keys,
   relative dots walk up from the importing file, `from m import a, b as c` emits per-name
   symbol stubs.
5. **`resolve_imports`**: `__init__.py` anchors its package directory (the `index.*` rule,
   extended); suffix matching also tries extension-stripped paths (a dotted Python spec has no
   extension to spell); two path forms of the same file are not ambiguity.
6. **`CallResolution.resolved_member_method`** counter, kept in `balances()`.

### Updated expectation
`cpp_extractor_test` pinned the old conservative rule with a fixture that is precisely the
true-positive case (`p.e->only_over_here(5)` uniquely naming a real method in another file).
The assertion flips to expect the INFERRED edge; two new negatives pin the guard rails (a
member call never binds to a free function of the same name, nor to an ambiguous method name).

## Impact

- Test reachability: gorilla/mux 0.11 → **0.50**, pallets/itsdangerous 0.07 → **1.00**
  (fresh binary, full extraction of each repo).
- Full smoke suite 68/68 including the new `test_connectivity_test` (Go cross-file receiver
  method resolves; free-function name does not; Python package imports resolve through
  `__init__.py`; `s.sign()` binds cross-file; no unresolved stubs remain).
- **Touches:** `language_config.{hpp,cpp}`, `extractor.cpp`, `configured_extractors.cpp`,
  `python_extractor.cpp`, `graph_builder.cpp`, `operation_stats.{hpp,cpp}`,
  `tests/smoke/{test_connectivity_test.cpp,cpp_extractor_test.cpp,CMakeLists.txt}`.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — member-call resolution, Python import extraction.
