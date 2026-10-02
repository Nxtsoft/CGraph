## 1. Header facts

- [x] 1.1 `header_contracts.hpp/.cpp`: `is_contract_header_name`; JavaScript/TypeScript, Python, Kotlin and Go walks emitting `uses_contract` / `provides_contract` with context `header:<name>`.
- [x] 1.2 One-line hooks in `javascript_extractor.cpp` (`js_extra_walk`), `python_extractor.cpp` (`python_extra_walk`), `configured_extractors.cpp` (`kotlin_http_walk`, `go_extra_walk`); `src/engine/CMakeLists.txt`.
- [x] 1.3 Standard headers, response headers and test sources record nothing; one same-file constant hop.
- [x] 1.4 Index version `logic-15`.

## 2. Verification

- [x] 2.1 `header_contracts_test.cpp` (five groups: TypeScript, TypeScript test file, Python, Kotlin, Go) fails with the `src` diff reverse-applied and passes after.
- [x] 2.2 Full suite.
- [x] 2.3 Probe: eight graphs against bin-v0.7.3, no node or edge lost or changed; every new cross-repo join hand-checked.
- [x] 2.4 Seam discover/fuse and scorer: T38, T39, T41, M33 link; no previously linked edge lost.

## 3. Review round 1 (PR #163)

- [x] 3.1 `resolve_contracts`: a header read in a function nothing calls, imports, references or routes to provides nothing unless bound (`kBoundHeaderRead`); `contract_reads_unreached` tally; `contracts_test` `test_unreached_header_reads`.
- [x] 3.2 Response words: camelCase `...Res` / `...Resp`, Elysia `set.headers`, Kotlin chain words without argument text.
- [x] 3.3 Reuse `js_syntax::unwrap_expression`, `is_function_node`, `parameter_names`; per-file `HeaderContractsFileScope` index of module constants and function parameters.
- [x] 3.4 A positive test per branch; a mutation run disabling each branch fails a test.
- [x] 3.5 Test-source rule: JVM `FooTest` / `FooTests` stems, `__mocks__`, `mocks`, `testutil`, `scripts/mock-*`, `conftest.py`.
- [x] 3.6 Header holders by exact name or suffix; constant hop respects shadowing (JavaScript, Python).
- [x] 3.7 Five more standard headers.
- [x] 3.8 Indexing time, base vs new, three runs.

## 4. Review round 2 and merge with main

- [x] 4.1 `reached` ignores edges from test sources (`is_test_source_path`, exported); `contracts_test` fixture adds `proxy.test.ts` importing and calling the unused reader.
- [x] 4.2 gin `GetHeader` is bound only on a `*gin.Context` parameter, else no fact; Ktor `call` is bound only when nothing else binds `call` or it is typed `ApplicationCall`.
- [x] 4.3 Merge origin/main (#161 env): both sides' extractor hooks kept; JS module-level senders use `js_syntax::reading_scope_id`; index version `logic-17`.
