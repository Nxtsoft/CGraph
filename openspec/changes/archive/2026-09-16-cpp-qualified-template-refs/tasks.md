# Tasks

- [x] 1.1 `cpp_extractor_test.cpp`: `std::vector<Payload>&`, `std::span<const Payload>` and
      `std::optional<Payload>` in a signature, and a `std::vector<Payload>` data member, each produce
      a `references` edge to `Payload` with `context: "generic_arg"`.
- [x] 1.2 `collect_type_refs` recurses into a qualified identifier's `template_type` name.
- [x] 2.1 `ctest --preset default`: 78/78 pass (this Mac, Debug preset).
- [x] 2.2 Before/after on CGraph's own tree: `generic_arg` references 0 -> 88; `RawCall` 0 -> 10 and
      `RawRelation` 0 -> 9 incoming; reference pairs 514 -> 591, none lost; unreferenced types 51 -> 45.
- [ ] 2.3 Obtain non-author review and merge; closes #94.
