## ADDED Requirements

### Requirement: Imports through JavaScript and TypeScript re-export barrels reach the declaration
When a JavaScript or TypeScript named import resolves to a file that does not declare the imported name, `resolve_imports` SHALL follow that file's re-exports to the declaration. It SHALL follow named re-exports (`export { a as b } from './x'`, matched by the exported name `b`) and wholesale re-exports (`export * from './x'`, excluding `export * as ns from`) breadth-first, for at most 8 hops, never revisiting a file. At each hop it SHALL look for the name the target knows the symbol by (after `export { a as b }`, it looks for `a`). A file that declares the name at module level (class members do not count), including the file the import first resolved to, SHALL stop the search: the import rebinds to its unique declaration, or to the file when it declares the name more than once, because a file's own exports shadow its star exports. A file's named re-export of the name SHALL shadow its star targets. The re-export marks of every statement SHALL survive edge deduplication, so `export { default } from './x'` together with `export * from './x'`, and two aliases of one name, all resolve. When no such symbol is reachable, the import SHALL stay on the file it first resolved to. Re-exports SHALL be read per importing file, so a module re-exported by several barrels resolves through each of them.

#### Scenario: Nested star barrels
- **GIVEN** `db/schema/index.ts` with `export * from './library'`, `library/index.ts` with `export * from './competitors'`, `competitors.ts` declaring `competitors`, and `import { competitors } from '../../db/schema'` in a service
- **THEN** the service has an `imports` edge to `competitors` in `competitors.ts`

#### Scenario: An aliased named re-export inside a star barrel
- **GIVEN** `library/index.ts` also has `export { widgets as gadgets } from './widgets'`, and the service imports `gadgets` from the schema barrel
- **THEN** the service has an `imports` edge to `widgets` in `widgets.ts`

#### Scenario: A star cycle terminates
- **GIVEN** `cyc/a/index.ts` with `export * from '../b'`, `cyc/b/index.ts` with `export * from '../a'`, and `import { missing } from './cyc/a'`
- **THEN** resolution terminates and the import's `imports` edge targets `cyc/a/index.ts`

#### Scenario: A two-hop alias chain
- **GIVEN** `chain/index.ts` with `export { baz as qux } from './inner'`, `inner.ts` with `export { bar as baz } from '../lib/b'`, and `import { qux } from './chain'`
- **THEN** the import's `imports` edge targets `bar` in `lib/b.ts`

#### Scenario: A local overload set shadows a star target
- **GIVEN** `mid.ts` declaring three `over` overloads and `export * from './deep'`, where `deep.ts` declares `over`, reached through a star barrel
- **THEN** the import rebinds to `mid.ts`, not to `deep.ts`'s `over`

#### Scenario: A named re-export before a star of the same module
- **GIVEN** `export { default } from '../m/x'` followed by `export * from '../m/x'`, and `import { named } from` that barrel
- **THEN** the import's `imports` edge targets `named` in `m/x.ts`
