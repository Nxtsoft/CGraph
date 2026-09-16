## ADDED Requirements

### Requirement: C-family relation targets resolve through transitive includes

When resolving a `references`, `inherits` or `implements` relation whose source file is C or C++, and the target is not an explicitly imported name, the engine SHALL search the declarations of every project file reachable from the source file through `imports`/`re_exports` edges between file nodes, breadth-first, to a depth of eight. The target SHALL resolve to the single declaration bearing its name at the nearest distance where any declaration bears it; when two declarations bear the name at that distance, or one file declares it twice, the relation SHALL be refused and no edge emitted; a declaration at a nearer distance SHALL shadow declarations farther away; a cycle among headers SHALL terminate. Relations from files in other languages SHALL continue to resolve through direct imports only.

#### Scenario: A type two includes away resolves
- **GIVEN** `app.cpp` includes `engine.hpp`, which includes `types.hpp`, which declares `Node`
- **WHEN** a function in `app.cpp` references `Node`
- **THEN** a `references` edge to `types.hpp`'s `Node` is emitted

#### Scenario: The nearer declaration shadows the farther one
- **GIVEN** `Config` declared in both `engine.hpp` (one include away) and `types.hpp` (two away)
- **THEN** the edge goes to `engine.hpp`'s `Config` only

#### Scenario: Two declarations at one distance are ambiguous
- **GIVEN** `Twin` declared in `types.hpp` and `other.hpp`, both two includes away
- **THEN** no `references` edge is emitted

#### Scenario: TypeScript stays direct-only
- **GIVEN** `app.ts` imports `mid.ts`, which imports `types.ts` declaring `Node`, and no import of `Node` in `app.ts`
- **THEN** a reference to `Node` from `app.ts` does not resolve
