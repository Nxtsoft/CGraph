# Fix: C/C++ type references resolve through transitive includes (CGR-4 follow-up)

## Why

`resolve_raw_relations` resolves a `references`/`inherits`/`implements` target through the
source file's imports and, for the C family, through the files it directly `#include`s. A C/C++
include is textual: everything a header includes is visible to the file that includes the header.
Resolving one hop only left CGraph's own `Node`, `Edge` and `RawCall` -- used in nearly every
engine file -- with zero incoming `references` edges, because `graph_builder.cpp` includes
`graph_builder.hpp`, which includes `types.hpp`. Measured in #83: 56 of the repo's 146 types
reported as `unreferenced` by `report types`, and a word-grep found 25 of them named in another
source file; `Node` had exactly one incoming edge, `contains` from its header.

The direct-include path also took the first included file that declared the name, in edge order:
an ambiguity was resolved by luck, not refused.

## What Changes

- For a relation whose source file is C or C++ (by `detect_language`), the include graph is walked
  breadth-first from the source file over `imports`/`re_exports` edges between file nodes, to a
  depth of 8, memoized per file. The target resolves at the nearest distance at which exactly one
  declaration bears its name. Two declarations at that distance (in two files, or one file that
  declares the name twice) refuse the edge; a nearer declaration shadows a farther one; header
  cycles terminate on the visited set.
- Other languages keep the direct-include path unchanged: a transitive import does not re-export
  names in TypeScript, Python or Rust.
- Tier order is unchanged: an explicit import of the name still wins, and heritage may still fall
  back to a same-file declaration.

### Non-goals
- `using`-directive or namespace-aware visibility. The walk is about which files' declarations are
  visible, not about scope; the CGR-4 scope gate on calls is a separate mechanism.
- Include-path resolution changes. Which project file an `#include` spec names is still decided by
  `resolve_imports`' suffix match.
- Precompiled or forced includes (`-include`, `stdafx.h`): not in the graph, so not walked.

## Impact

- **Touches:** `src/engine/graph_builder.cpp`, `tests/smoke/graph_builder_test.cpp`,
  `tests/smoke/cpp_extractor_test.cpp`.
- `graph.json` gains `references`/`inherits` edges in C/C++ projects; every other language is
  byte-identical.
- Measured on CGraph's own tree (193 files, 2,022 nodes, same source, before = origin/main binary,
  after = this change): `references` edges 225 -> 542; `Node` 0 -> 34 incoming references,
  `GraphSnapshot` 3 -> 99, `Edge` 0 -> 6; `report types`' unreferenced set (tests excluded) 60 -> 51;
  build time unchanged at ~1 s. `RawCall` stays at 0: in signatures it appears only as a template argument
  (`std::span<const RawCall>`, `std::vector<RawCall>&`), and the C++ extractor's generic-argument
  reference emission does not reach it -- a separate follow-up on the extractor, not on resolution.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — C-family relation targets resolve through transitive includes, nearest unique declaration.
