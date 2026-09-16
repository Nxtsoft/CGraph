# Tasks

- [x] 1.1 `graph_builder_test.cpp::test_resolve_relations_through_includes`: two-hop resolution; a
      nearer declaration shadows a farther one; two declarations at one distance are ambiguous and
      refuse the edge; depth 3 resolves; a header cycle terminates; TypeScript stays direct-only.
- [x] 1.2 `cpp_extractor_test.cpp`: `consumer.cpp` -> `engine.hpp` -> `types.hpp` resolves `Payload`
      through real extraction.
- [x] 1.3 `resolve_raw_relations`: per-file include levels (BFS, depth 8, memoized), nearest-unique
      resolution for C-family sources; other languages unchanged.
- [x] 2.1 `ctest --preset default`: 78/78 pass (this Mac, Debug preset).
- [x] 2.2 Before/after on CGraph's own tree (2,022 nodes): `references` edges 225 -> 542; incoming
      references `Node` 0 -> 34, `GraphSnapshot` 3 -> 99, `Edge` 0 -> 6, `CallResolution` 0 -> 1
      (`RawCall` still 0: in signatures it appears only as a `std::span<const RawCall>` / `std::vector<RawCall>&`
      template argument, a follow-up on the C++ extractor's generic-argument references); types with no
      non-structural incoming edge, tests excluded, 60 -> 51; one-shot build 0.98 s.
- [ ] 2.3 Obtain non-author review and merge.
