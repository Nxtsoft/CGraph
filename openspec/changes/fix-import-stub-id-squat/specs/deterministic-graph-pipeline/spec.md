## ADDED Requirements

### Requirement: Import stub identities are disjoint from real node identities
Import and module stub nodes emitted during extraction SHALL derive their ids in a reserved
namespace that no extracted file or symbol node can occupy, so that fragment merging can
never discard a real node in favor of a stub regardless of merge order or import spelling
(extensioned or extensionless, relative or bare). Stubs SHALL continue to be fully consumed
by import resolution — remapped onto the real file or declared symbol, or dropped — so no
stub id reaches an export.

#### Scenario: An extensioned relative import cannot erase the imported file
- **GIVEN** `a.spec.ts` importing `{ chunkBy } from './chunkBy.ts'` alongside `chunkBy.ts`,
  with the importer merging first
- **WHEN** the one-shot pipeline runs
- **THEN** the graph contains `chunkBy.ts`'s file node and its `chunkBy` symbol node, and
  the spec's `imports` edge resolves to that symbol

#### Scenario: Stub and real ids never collide at extraction
- **WHEN** a file importing `./x.ts` and the file `x.ts` are extracted
- **THEN** no stub node in the importer's fragment shares an id with any node in `x.ts`'s
  fragment

#### Scenario: Shared-import hub collapse still works
- **WHEN** many files import the same symbol from the same module
- **THEN** their stubs share one id and collapse onto one hub node before resolution
