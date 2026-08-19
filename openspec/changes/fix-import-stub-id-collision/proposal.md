# Fix: JS/TS import stubs must never share an id with a real node (issues #39, #40)

## Why

On toss/es-toolkit@`1a629d17`, extraction silently deleted **650 of 1,508 `.ts` files**
(no file node, no symbol nodes; issue #39) and left surviving import edges pointing at the
wrong basename twin (issue #40). Root cause, reduced to a two-file reproducer
(`chunkBy.ts` + `chunkBy.spec.ts`):

1. `resolve_module_spec` absolutizes relative specifiers. When a specifier spells the real
   extension — `import { chunkBy } from './chunkBy.ts'`, legal under
   `allowImportingTsExtensions` and used throughout es-toolkit — the resolved string is the
   imported file's exact source path.
2. The module stub's id was `make_id(resolved)` — **identical to the real file node's id**
   (`make_id(source_file)`). The symbol stub's id, `make_id(module_key + ":" + name)`, was
   likewise identical to the real declared symbol's id.
3. Fragments merge in path order and `X.spec.ts` sorts before `X.ts`, so the spec's stubs
   claimed the ids first; `merge_fragment` discarded the real file and function nodes as
   duplicates.
4. `resolve_imports` then found no `file`-kind node for the stub's path (the id it occupies
   has kind `module`), classified it as an unresolvable third-party import, and deleted the
   stub with every edge — erasing the file, its symbols, and its dependents from the graph.

Extension-less specifiers never collide (their resolved string lacks the extension), which is
why the damage was invisible on repos that don't use extension imports.

## What Changes

- `javascript_extractor.cpp::module_import_handler`: namespace both stub ids so they live in
  an id space no real node can occupy — `make_id("import-module:" + resolved)` and
  `make_id("import-symbol:" + module_key + ":" + name)`. Stubs are pre-merge scaffolding:
  `resolve_imports` remaps every resolvable stub onto the real node (computing the real id
  from the file node, independent of the stub id) and deletes the rest, so no exported id
  changes on healthy graphs.
- New smoke test `import_stub_collision_test.cpp`: the two-file reproducer plus an
  extension-less control; asserts the imported file and function survive, no stub remains,
  and both import styles resolve onto the real nodes.

### Non-goals
- Other extractors' stubs (C++, Go/Rust configured, Python): their ids are built from raw
  relative specs, never an absolutized source path, so the collision class cannot occur.
- Any change to `resolve_imports` matching semantics.

## Impact

- es-toolkit-class repos (extension imports, basename twins across package halves) regain
  their deleted files, symbols, and edges; downstream impact/dependents queries stop being
  blind there. Verified against es-toolkit before/after in the fix PR.
- **Touches:** `src/engine/javascript_extractor.cpp`, `tests/smoke/import_stub_collision_test.cpp`,
  `tests/smoke/CMakeLists.txt`.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — JS/TS import-stub identity.
