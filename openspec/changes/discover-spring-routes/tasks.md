## 1. Tests first

- [x] 1.1 `contracts_test.cpp` `test_spring_mappings`: Kotlin and Java controllers plus a TypeScript client, through extraction, merge, imports and contracts. Expected: 10 endpoints, canonical ids, `handled_by` methods, a `CONSUMES` edge, and nothing minted for a constant path or a method-less `@RequestMapping`.
- [x] 1.2 `configured_extractors_test.cpp`: the Kotlin extractor emits `file_route` facts with the joined path and lowercase verb.

## 2. Implementation

- [x] 2.1 A text parser for mapping annotations (positional, `value =`, `path =`, arrays, `method =`) and `kotlin_relation_handler` / `java_relation_handler` in `configured_extractors.cpp`.

## 3. Verification

- [x] 3.1 Mutation check: without the class prefix, and without `method =` parsing, the test fails.
- [x] 3.2 Full suite: `ctest --test-dir build/default -j4`.
- [x] 3.3 Real flow: `build/default/src/cli/cgraph --root ~/idp/idp-core --out <dir>`. Count endpoint nodes and compare them with idp-core's springdoc export (`~/modsquad/passless-app/composeApp/openapi/v3-api-docs.json`).
- [x] 3.4 `openspec validate discover-spring-routes --strict`.
