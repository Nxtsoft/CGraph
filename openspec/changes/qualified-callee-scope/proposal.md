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
  which collects each `qualified_identifier` scope while descending to the leaf. A scope that
  names a type parameter of an enclosing template voids the whole qualifier (`T::make()` names
  whatever T is instantiated with, which no declaration carries); a scope that names a namespace
  alias is rewritten to the namespace itself (`pd::helper()` under `namespace pd = proj::detail`
  is gated as `proj::detail::helper()`, and `fs::exists` under `namespace fs = std::filesystem`
  is refused as the `std::` call it is).
- `cpp_field_walk` stamps a `scope` property on every function, class, struct, union and enum
  node declared inside a namespace: the `::`-joined enclosing namespace names, `(anonymous)` for
  an unnamed namespace, plus whatever the declarator itself qualifies -- an out-of-line
  definition names its owner and nothing else records it (`int proj::Cache::reload() {}` carries
  `proj::Cache`; the in-class prototype is a `field_declaration` with no node of its own). An
  inline namespace contributes no segment, because qualified lookup sees through it. Nothing is
  stamped at file scope, so a namespace-free graph is byte-identical to before.
- `resolve_raw_calls`, tier 2b (member call, unknown receiver, unique project method): a bare
  name every standard library defines on its containers, strings, iterators, smart pointers and
  option types (`size`, `find`, `empty`, `begin`, `value`, `unwrap`, ...) is refused. The check is
  on the make_id-normalized key, so `.Count()` and `.count()` both match. It is counted as
  `dropped_library_member` only when a project method existed to refuse; otherwise the call stays
  `dropped_unknown`. Names a project plausibly owns (`open`, `get`, `add`, `apply`) are
  deliberately not on the list.
- `resolve_raw_calls`, after the existing tiers produce a target (and any overload siblings) and
  before edges are emitted: when the call carries a qualifier, every candidate is kept only if
  the qualifier's segments are a suffix of its `scope` segments (anonymous namespaces are
  transparent), or it is a method of a class bearing the last segment whose own scope carries
  the rest (`proj::Stats::size_of()`). Segments, never text: a template scope `Outer<proj::Beast>`
  is recorded as `Outer`. A set with no survivor is refused and counted as the new
  `dropped_scope_mismatch`, which `CallResolution::balances()` includes.
- The gate refuses a CONTRADICTION, never an absence of evidence: it runs only where the
  qualifier has something to contradict -- its outermost segment is `std`, or at least one
  candidate records a `scope` or an owning class. A call whose candidates record neither is a
  qualifier checked against nothing, and binds as an unqualified call would. Requiring positive
  proof instead dropped every call to an out-of-line member definition, a nested class, a
  namespace alias, an inline namespace and a dependent `T::make()`.

### Non-goals
- `using namespace` directives. An unqualified call keeps the ordinary rules.
- Rust and Go keep the defect. `rust_config` sets no `resolve_callee_scope` and `rust_callee_name`
  (`src/engine/configured_extractors.cpp:542-545`) descends `scoped_identifier` to its leaf, so
  `std::mem::swap(x, y)` still binds to a project `swap`; Go selectors reduce the same way. Wiring
  either is tracked separately, not done here.
- A call that spells an inline namespace explicitly (`proj::v1::versioned()`). The declaration
  records `proj`, the spelling most code uses; the explicit one no longer matches.

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

Follow-up (the review of #76, CGR-4): making the gate refuse only a contradiction changes nothing
on this repo -- origin/main's `src/` (104 files, 1300 nodes) resolves to a byte-identical partition
before and after (`resolved_project_unique` 403, `resolved_member_method` 24,
`dropped_library_member` 616, `dropped_scope_mismatch` 18, 940 CALLS edges, partitions balance),
because every mismatch here is a genuine `std::` contradiction. What it recovers is the six shapes
an arbitrary C++ repo is made of, each asserted in `cpp_extractor_test.cpp` and each failing
before the fix: an out-of-line member definition, a nested class, a namespace alias, an inline
namespace, a dependent `T::make()`, against the in-class control that already bound.

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
