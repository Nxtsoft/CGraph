## ADDED Requirements

### Requirement: Imports through JavaScript and TypeScript re-export barrels reach the declaration
When a JavaScript or TypeScript named import resolves to a file that does not declare the imported name, `resolve_imports` SHALL follow that file's re-exports to the declaration. It SHALL follow named re-exports (`export { a as b } from './x'`, matched by the exported name `b`) and wholesale re-exports (`export * from './x'`, excluding `export * as ns from`) breadth-first, for at most 8 hops, never revisiting a file. It SHALL rebind the import to the first symbol found that a file declares exactly once, or that a named re-export targets. When no such symbol is reachable, the import SHALL stay on the file it first resolved to. Re-exports SHALL be read per importing file, so a module re-exported by several barrels resolves through each of them.

#### Scenario: Nested star barrels
- **GIVEN** `db/schema/index.ts` with `export * from './library'`, `library/index.ts` with `export * from './competitors'`, `competitors.ts` declaring `competitors`, and `import { competitors } from '../../db/schema'` in a service
- **THEN** the service has an `imports` edge to `competitors` in `competitors.ts`

#### Scenario: An aliased named re-export inside a star barrel
- **GIVEN** `library/index.ts` also has `export { widgets as gadgets } from './widgets'`, and the service imports `gadgets` from the schema barrel
- **THEN** the service has an `imports` edge to `widgets` in `widgets.ts`

#### Scenario: A star cycle terminates
- **GIVEN** `cyc/a/index.ts` with `export * from '../b'`, `cyc/b/index.ts` with `export * from '../a'`, and `import { missing } from './cyc/a'`
- **THEN** resolution terminates and the import's `imports` edge targets `cyc/a/index.ts`
