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

## 6. Refuse only a contradiction (review of #76)
- [x] 6.1 `cpp_extractor_test.cpp`: six pre-failing assertions -- out-of-line member definition,
      nested class, namespace alias, inline namespace, dependent `T::make()`, and the in-class
      control that already bound -- plus the `scope` property `proj::Cache` on an out-of-line
      definition. All six fail against origin/main's engine, verified by building the test against
      it.
- [x] 6.2 `graph_builder_test.cpp::test_qualified_scope_no_evidence`: a candidate recording
      neither scope nor owner binds a project-qualified call and still refuses a `std::` one.
- [x] 6.3 `stamp_namespace_scope` appends `declarator_scope`; inline namespaces contribute no
      segment; `cpp_callee_scope` voids a dependent qualifier and rewrites a namespace alias.
- [x] 6.4 The gate runs only when the qualifier can contradict: root `std`, or a candidate that
      records a `scope` or an owning class.
- [x] 6.5 `owner_id_by_method` is built in the `method`-edge walk that already fills
      `methods_by_owner`, not a second pass over `graph.edges`.
- [x] 6.6 Full suite: 76/77 pass; `cgraph_file_watcher_test` fails on this host with tmpfs `/tmp`
      before and after this change (unrelated, noted on the PR).
- [x] 6.7 Before/after on origin/main `src/` (104 files): every `call_resolution` field identical,
      940 CALLS edges either way, partitions balance.

## 7. Derive the contradicting roots from the project (review of #80)
- [x] 7.1 `graph_builder_test.cpp::test_library_qualified_calls`: `fmt::format`, `boost::algorithm::trim`,
      `absl::strings_internal::size`, `QString::find` and `std::filesystem::remove` against file-scope
      project functions of the same name emit no edge, while `proj::detail::helper()` still resolves.
      Four of the same shapes asserted through real extraction in `cpp_extractor_test.cpp`; all fail
      with the `std`-only allowlist in place.
- [x] 7.2 `project_scope_roots`: the outermost segment of every `scope` property. A qualifier rooted
      outside that set contradicts every candidate; `kStdNamespace` is deleted.
- [x] 7.3 `test_qualifier_root_unknown_to_project` (was `test_qualified_scope_no_evidence`, whose
      synthetic project records no scope at all, so `proj::Cache::reload()` is a library call by this
      rule) and `test_qualifier_root_is_project_namespace` (the root IS the project's, the candidate
      records nothing, the call binds and is tallied `resolved_qualifier_unchecked`).
- [x] 7.4 `CallResolution::resolved_qualifier_unchecked`, serialized in `operation_stats.cpp`, a
      subset of the resolved fields like `resolved_overload_first`.
- [x] 7.5 Alias rewriting stated as file-local in the spec and proposal; the header case is an
      explicit non-goal.
- [x] 7.6 Full suite and before/after on origin/main `src/` re-run; every field unchanged and
      `resolved_qualifier_unchecked` 0.
