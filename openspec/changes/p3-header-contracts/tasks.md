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
