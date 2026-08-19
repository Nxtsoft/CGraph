## Why

Issues #39 and #40: on toss/es-toolkit at 1a629d17, the one-shot build loses **650 of 1508
`.ts` files** — no file node, no symbol nodes (1508 files → 2268 nodes / 1667 edges,
~1.1 edges/file) — and the surviving import edges include same-dir imports dropped and a
spec's edge pointing at the wrong basename twin.

Root cause (reproduced down to a two-file case: `chunkBy.ts` + `chunkBy.spec.ts` → the
implementation vanishes entirely): the JS/TS extractor derives import stub ids as
`make_id(resolved_spec)` and `make_id(resolved_spec + ":" + name)`. With extensioned
relative imports (`from './chunkBy.ts'`, the es-toolkit house style) the resolved spec IS
the imported file's path, so those ids are byte-identical to the real FILE node id and the
real SYMBOL node id. `merge_fragments` keeps the first occurrence per id and files merge in
sorted order — `chunkBy.spec.ts` sorts before `chunkBy.ts` — so the importer's stubs squat
the real ids, the real nodes are discarded at merge, and `resolve_imports` then drops the
now-unresolvable stubs, erasing the imported file and its symbols from the graph. Issue
#40's wrong-twin and dropped-edge symptoms are the downstream cascade: with the real file
node squatted away, later imports of it fall through exact lookup to ambiguous suffix
matching.

The observable contract tests verify: import stub ids never collide with real node ids, and
the es-toolkit build recovers every source file.

## What Changes

- Namespace the JS/TS import stub ids so they can never equal a real node's id:
  `make_id("js_module:" + resolved)` and `make_id("js_import:" + module_key + ":" + name)` —
  the same convention the Rust `use` resolver already established (`rust_use:` stubs).
  Stubs are always consumed by `resolve_imports` (remapped onto the real file/symbol or
  dropped), so the namespace never reaches an export and graph.json parity is unaffected.
  `import_path` properties are unchanged; resolution logic is untouched.
- Regression test: an extensioned relative import's stubs must share no id with the
  imported file's real nodes.
- Non-goals: TS path-alias/`tsconfig` resolution changes; the suffix-match ambiguity rule
  (unchanged: zero-or-unique); Kotlin/Python import gaps.

## Impact

- `src/engine/javascript_extractor.cpp` — two id derivations.
- `tests/smoke/javascript_extractor_test.cpp` — collision regression block.
- Capability: `deterministic-graph-pipeline`. Measured on es-toolkit@1a629d17: file nodes
  858 → 1508 (all present), edges/file recovers from ~1.1 to the healthy range, and the
  issue-#40 spec's imports resolve to the correct compat twins (verified in the PR).
