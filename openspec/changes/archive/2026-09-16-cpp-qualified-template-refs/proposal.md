# Fix: template arguments of namespace-qualified C++ types are references (#94)

## Why

After #92 made C-family references resolve through transitive includes, `RawCall` and
`RawRelation` still had exactly one incoming edge each -- `contains` from their header -- and the
whole 2,022-node CGraph graph carried zero `references` edges with `context: "generic_arg"`
(against 388 `parameter_type`, 127 `return_type`, 27 `field`).

`collect_type_refs` in `cpp_extractor.cpp` treats a `qualified_identifier` as a leaf: it emits
the tail of the qualified text and returns. tree-sitter-cpp parses `std::span<const RawCall>`
as `qualified_identifier(scope: std, name: template_type(span, <const RawCall>))`, so the
`template_type` branch that walks the argument list with `generic = true` is never reached for a
namespace-qualified template. Every signature in this engine that names `RawCall` has that shape
(`std::span<const RawCall>` in `graph_builder.hpp`, `std::vector<RawCall>&` in
`language_config.hpp` and the extractor hooks). An unqualified `vector<RawCall>` would have
worked, which is why the gap never showed in the fixture.

## What Changes

- In the `qualified_identifier` branch, when the `name` child is a `template_type` (or a further
  `qualified_identifier`), `collect_type_refs` recurses into it instead of emitting the qualified
  text. The template's base name is emitted as before (unresolvable library names such as `span`
  are dropped at resolution, as `vector` already was) and each argument is walked as a generic
  argument, so `std::vector<Payload>&`, `std::span<const Payload>` and `std::optional<Payload>`
  each yield a `references` edge to `Payload` with `context: "generic_arg"`.
- `cpp_extractor_test.cpp` adds `generics.cpp` (a function with those three shapes and a struct
  with a `std::vector<Payload>` member) and asserts the edges and their context through real
  extraction.

### Non-goals
- Non-type template arguments, dependent names (`typename T::value_type`) and alias templates.
- Any change to resolution: the new relations go through the same tiers as every other reference.

## Impact

- **Touches:** `src/engine/cpp_extractor.cpp`, `tests/smoke/cpp_extractor_test.cpp`.
- `graph.json` gains `references` edges in C++ projects that use qualified templates; every other
  language is byte-identical.
- Measured on CGraph's own tree (193 files, 2,024 nodes; before = the #92 graph, after = this
  change): `generic_arg` references 0 -> 88; `RawCall` 0 -> 10 incoming references, `RawRelation`
  0 -> 9, `Node` 34 -> 41, `Fragment` 37 -> 39; distinct (source, target) reference pairs 514 ->
  591 with none lost (the 10 fewer `parameter_type`/`return_type` edges are the same pairs now
  tagged `generic_arg`, since one edge per pair is kept); `report types`' unreferenced set (tests
  excluded) 51 -> 45. Build time unchanged (1.2 s).

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — template arguments of qualified C++ types are references.
