# Tasks

## 1. Root-cause (issues #39 + #40, one mechanism)

- [x] 1.1 Reproduced #39 byte-exact on es-toolkit@1a629d17 (1508 files → 2268 nodes,
      650 file nodes missing) and bisected to a two-file repro: `chunkBy.ts` +
      `chunkBy.spec.ts` → the implementation file and its symbol vanish.
- [x] 1.2 Traced the mechanism: extensioned import specs make the naive stub ids
      (`make_id(resolved)`, `make_id(resolved + ":" + name)`) byte-identical to the real
      file/symbol node ids; first-occurrence-wins merge lets the importer's stubs squat
      them; resolve_imports then drops the unresolvable stubs. #40's dropped/wrong-twin
      edges are the downstream cascade of the squatted file nodes.

## 2. Fix (red first)

- [x] 2.1 javascript_extractor_test: an extensioned relative import's stubs must share no
      id with the imported file's real nodes. Red against the shipped extractor, green
      after.
- [x] 2.2 Namespace the stub ids: `js_module:` / `js_import:` (the `rust_use:` convention).
      import_path properties and all resolution logic unchanged.

## 3. Prove it end-to-end

- [x] 3.1 es-toolkit@1a629d17 rebuild with the fix: 0 of 1508 `.ts` files missing (was
      650); 2268 -> 3556 nodes, 1667 -> 9793 edges (1.1 -> 6.5 edges/file). Issue #40's
      spec resolves all four imports exactly: `./debounce` ->
      `src/compat/function/debounce.ts` (the correct twin) and identity/noop/delay to
      their non-compat files; no dropped or wrong-twin edges.
- [x] 3.2 Full `ctest --preset default` green (66/66); retrieval gate pins hold. Sanitizers
      via CI (local ASan hang recorded in add-rust-import-resolution).
