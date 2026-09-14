# Fix: a qualified callee's scope must agree with the resolved declaration (CGR-4)

## Why

`resolve_raw_calls` keys a call on the callee's leaf name. For a qualified callee the C-family
resolver descends `qualified_identifier` to that leaf and discards the scope, so `std::find(...)`
is keyed as `find` and `std::filesystem::exists(p)` as `exists`. The scope was never carried to
resolution, yet it is the one fact at the call site that says which declaration the call means.

The consequence on CGraph's own source (installed 0.1.0, origin/main 4a31f3e): a project function
named `find` in `src/engine/dedup.cpp` received a `CALLS` edge from every `std::find` in the tree,
and the same shape turns every `std::` or `fs::` call into a dependent of any same-named project
symbol. Each such edge is a false dependent in `impact`, which is exactly what CGR-4 asks to stop.

The exactly-one-candidate rule is right and is not relaxed. The defect is that a qualifier is
evidence, and it was thrown away before the rule ran.

## What Changes

- `RawCall` carries `qualifier`: the scope text of a qualified callee as written (`std`,
  `proj::detail`), empty for an unqualified or member callee.
- `LanguageConfig::resolve_callee_scope`, implemented for the C family by `cpp_callee_scope`,
  which collects each `qualified_identifier` scope while descending to the leaf.
- `cpp_field_walk` stamps a `scope` property on every function, class, struct, union and enum
  node declared inside a namespace: the `::`-joined enclosing namespace names, `(anonymous)` for
  an unnamed namespace. Nothing is stamped at file scope, so a namespace-free graph is
  byte-identical to before.
- `resolve_raw_calls`, tier 2b (member call, unknown receiver, unique project method): a bare
  name every standard library defines on its containers, strings, iterators, smart pointers and
  option types (`size`, `find`, `empty`, `begin`, `value`, `unwrap`, ...) is refused and counted as
  `dropped_library_member`. Names a project plausibly owns (`open`, `get`, `add`, `apply`) are
  deliberately not on the list.
- `resolve_raw_calls`, after a target is chosen by the existing tiers and before the edge is
  emitted: when the call carries a qualifier, the target's `scope` must end with the qualifier's
  innermost segment, or the target must be a method of a class bearing that name
  (`proj::Stats::size_of()`). Otherwise the call is refused and counted as the new
  `dropped_scope_mismatch`, which `CallResolution::balances()` includes.

### Non-goals
- Namespace aliases (`namespace fs = std::filesystem`): `fs::exists` resolves to nothing, which is
  correct for a project that declares no `fs` namespace, and a project that does is not helped.
- `using namespace` directives. An unqualified call keeps the ordinary rules.
- Other languages. Rust paths and Go selectors already go through the member-call tiers.

## Impact

Measured on origin/main `src/` (102 files, 1215 nodes) with the origin/main binary and this branch,
same machine, same source:

| Measure | Before | After |
|---|---:|---:|
| CALLS edges | 1,095 | 874 (221 removed, 0 added) |
| Incoming CALLS on `operation_stats.hpp::size` | 141 | 0 |
| Incoming CALLS on `dedup.cpp::find` | 71 | 0 |
| `resolved_member_method` | 551 | 15 |
| `dropped_library_member` | - | 2,067 |
| `dropped_scope_mismatch` | - | 17 |
| Partitions balance | yes | yes |

Removed edges by target label: `size` 139, `find` 65, `remove` 7, `ends_with` 5, `transform` 4,
`status` 1. Each is a `std::` or `fs::` call that had been attached to a same-named project
symbol. Two runs produce a byte-identical `graph.json`.

The cost is stated plainly: a project method that shares a standard-library member name
(`Cache::size()`) is no longer reachable from a cross-file `x.size()` with an unknown receiver.
Same-file calls and receiver-named calls (`Stats::size()`) still bind. Recall on the eight
benchmark repos is the follow-up measurement for CGR-4 before the list is widened.

- **Touches:** `src/engine/include/cgraph/language_config.hpp`, `src/engine/extractor.cpp`,
  `src/engine/include/cgraph/cpp_extractor.hpp`, `src/engine/cpp_extractor.cpp`,
  `src/engine/configured_extractors.cpp`, `src/engine/include/cgraph/operation_stats.hpp`,
  `src/engine/operation_stats.cpp`, `src/engine/graph_builder.cpp`,
  `tests/smoke/graph_builder_test.cpp`, `tests/smoke/cpp_extractor_test.cpp`.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — call resolution requires a qualified callee's scope to agree
  with the resolved declaration; C-family symbols declared in namespaces carry a `scope` property.
