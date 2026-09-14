# Tasks

## 1. Carry the qualifier
- [x] 1.1 `RawCall::qualifier` and `LanguageConfig::resolve_callee_scope`.
- [x] 1.2 `cpp_callee_scope`: collect every `qualified_identifier` scope, descending through
      `template_function` / `template_method`, joined with `::`.
- [x] 1.3 `add_raw_call` fills `qualifier` whenever the config supplies the hook.

## 2. Stamp the declaration's scope
- [x] 2.1 `stamp_namespace_scope` in `cpp_field_walk`: enclosing `namespace_definition` names,
      outermost first, `(anonymous)` for an unnamed namespace, on the node the walk just added.
- [x] 2.2 No property at file scope, so namespace-free fixtures and goldens are unchanged.

## 3. Refuse a scope mismatch
- [x] 3.1 Gate in `resolve_raw_calls` after target selection: `scope` ends with the innermost
      qualifier segment, or the target is a method of a class with that name.
- [x] 3.2 `CallResolution::dropped_scope_mismatch`, included in `balances()` and in the stats JSON.

## 4. Test
- [x] 4.1 `graph_builder_test.cpp::test_qualified_scope`: `std::find` refused, `proj::detail::helper`
      resolved, `proj::Stats::size_of` resolved through the class, a same-file `exists` not captured
      by `std::filesystem::exists`, the unqualified call unchanged, partitions balance.
- [x] 4.2 `cpp_extractor_test.cpp`: the same shapes through real tree-sitter extraction, plus the
      `scope` property on a symbol declared in nested namespaces.
- [x] 4.3 Full suite: 75/76 pass; `cgraph_file_watcher_test` fails on this host with tmpfs `/tmp`
      before and after this change and passes with `TMPDIR` on ext4 (unrelated, noted on the PR).

## 5. Measure
- [x] 5.1 Before/after on origin/main `src/` (102 files, 1215 nodes), origin/main binary vs this
      branch: CALLS 1095 -> 874 (221 removed, 0 added); incoming on `operation_stats.hpp::size`
      141 -> 0, on `dedup.cpp::find` 71 -> 0; removed edges by target: size 139, find 65, remove 7,
      ends_with 5, transform 4, status 1 (every one a `std::`/`fs::` call); `resolved_member_method`
      551 -> 15, `dropped_library_member` 2067, `dropped_scope_mismatch` 17, partitions balance.
- [x] 5.2 Determinism: two runs produce a byte-identical `graph.json`.
