# Tasks

- [x] Diagnose why `new Foo()` produces no call edge (callee is `object_creation_expression.type`,
  a `_simple_type`, not the `name` field java_config read).
- [x] Add `java_callee_name` reducing `type_identifier` / `generic_type` / `scoped_type_identifier`
  to the bare simple class name; pass a `method_invocation` `name` through unchanged.
- [x] Read the constructor callee via `call_accessor_fields = {"name", "type"}`.
- [x] Add `check_java_extraction` covering plain, generic, and package-qualified constructors plus an
  unchanged plain method call.
- [x] Verify: smoke tests pass, no other-language regressions, and a real Java repo clears the
  reachability floor (0.241 -> 0.847 on JSON-java).
