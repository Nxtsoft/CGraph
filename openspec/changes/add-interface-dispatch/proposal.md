# Add: interface-dispatch resolution (Go)

## Why

After #46, member calls resolve project-wide when the bare name uniquely names a method — but
idiomatic Go defeats uniqueness by design: gorilla/mux declares `Match` on **eight** types
because they all satisfy one `matcher` interface. An ambiguous member call was dropped
(honestly), so tests exercising `Route.Match` never appeared in its dependents; blastline's
replay still missed 4 of 11 author-co-changed tests, all of this shape.

## What Changes

1. **Go receiver attribution** (`go_relation_handler`): `func (r *Route) Match(...)` emits a
   `method_of` raw relation (method → receiver type name), resolved by
   `resolve_raw_relations` (same-file allowed — Go's common layout). 84 edges on mux.
2. **Go interface method sets** (`go_extra_walk`): each `method_elem` of an `interface_type`
   becomes a function node tagged `interface_method`, owned by the interface's type node via a
   `method` edge. Namespaced ids — a contract entry is never an implementation's node.
3. **`resolve_interface_dispatch`** (new pass, after `resolve_raw_relations`, wired into both
   the one-shot pipeline and the incremental fold-in):
   - `implements`: T satisfies I when I's method-name set ⊆ T's (name-only, structural-lite),
     graded INFERRED.
   - `dispatches_to`: each interface method → every implementation's same-named method.
   - **Member-call rescue**: a member call whose bare name is ambiguous among concrete
     methods but names **exactly one interface method project-wide** binds to the contract
     node (INFERRED). Dependents then flow contract → implementations, so "what breaks if
     `Route.Match` changes" finally includes the tests that call `.Match(...)`.

### Non-goals
- Embedded interfaces (interface composition) — recorded follow-up.
- Signature-aware satisfaction (name-only in v1; the exactly-one rescue rule is the honesty
  guard, not subset math).
- Cross-package receiver resolution beyond what `resolve_raw_relations` already reaches.

## Impact

- gorilla/mux: test reachability 0.50 → **0.65**; relations gained `method_of` (84),
  `implements` (9), `dispatches_to` (9); `Route.Match` now carries incoming dispatch from the
  `matcher` contract, which tests call.
- Full smoke suite 69/69, including new `interface_dispatch_test` (contract materializes;
  both implementations get `implements`/`dispatches_to`; ambiguous member call binds to the
  contract; a name no interface promises stays dropped).
- **Touches:** `configured_extractors.cpp`, `graph_builder.{cpp,hpp}`, `pipeline.cpp`,
  `incremental_update.cpp`, `tests/smoke/{interface_dispatch_test.cpp,CMakeLists.txt}`.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — interface dispatch edges and member-call rescue.
