## Why

Java constructor calls (`new Foo()`) produced no call edge. In `method_invocation` the callee is
the `name` field, but in `object_creation_expression` it is the `type` field — a `_simple_type`, not
a bare identifier — and `java_config` only read `name`, so every `new Foo()` fell through to the
verbatim-text path and matched nothing. Because `contains`/`method` edges make a class's methods
reachable from the class node, an unresolved constructor severs the test → class → methods path that
carries most of Java's test-to-code reachability. Measured on a real repo (stleary/JSON-java, a
constructor-heavy JUnit codebase): test→implementation reachability was **0.241**, just under
blastline's 0.25 disconnected-tests floor — so a downstream selector fails open on the whole repo.

## What Changes

- Add `java_callee_name` (a `resolve_callee_name`) that reduces a constructor's `type` node to the
  bare simple class name: `type_identifier` verbatim, `generic_type` (`Foo<T>`) to its base type,
  `scoped_type_identifier` (`a.b.Foo`) to its last simple name. A `method_invocation`'s `name`
  identifier passes through unchanged.
- Read the constructor callee: `call_accessor_fields = {"name", "type"}` so
  `object_creation_expression.type` is picked up alongside `method_invocation.name`.

This is purely additive: method-call resolution is unchanged (the `name` path and non-member grading
are identical), so only `new Foo()` edges are added. A constructor call is non-member and resolves
project-wide to the class of that name, exactly like any other unique-name call.

### v1 non-goal (recorded follow-up)
- **Interface/polymorphic dispatch.** A method name shared between an interface and its implementors
  (or across sibling classes) is still dropped as ambiguous — the gap `#47` closed for Go with
  `implements`/`dispatches_to` and the member-call rescue. Java's grammar reuses `method_declaration`
  for interface methods (unlike Go's distinct `method_elem`), so tagging them as `interface_method`
  contract nodes without duplicating the plain method node is the design work involved. Deferred:
  constructor resolution alone clears the reachability floor with wide margin (see Impact).

## Impact

- `new Foo()`, `new Foo<T>()`, and `new pkg.Foo()` now produce a `CALLS` edge to the class node,
  which (through `method`/`contains`) reconnects tests to the classes they construct. Verified
  end-to-end: JSON-java reachability rose **0.241 → 0.847** (resolved edges 3,632 → 11,693),
  clearing the disconnected-tests floor so a selector produces real subsets instead of failing open.
- **Touches:** `src/engine/configured_extractors.cpp` (`java_callee_name` + `java_config`),
  `tests/smoke/configured_extractors_test.cpp` (`check_java_extraction`). No new translation unit.

## Capabilities

### Modified Capabilities

- `deterministic-graph-pipeline` — Java constructor-call resolution.
