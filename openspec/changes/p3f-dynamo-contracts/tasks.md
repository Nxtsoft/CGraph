## 1. Foundation

- [x] 1.1 `contracts.hpp/.cpp`: `dynamo` contract kind, `dynamo:<name>` (case-sensitive, no database), bridged by name; docs.
- [x] 1.2 `resolve_contracts` step 8: a DynamoDB fact's `target_label` env variables become the node's `env` property.
- [x] 1.3 Dedup exemption through `is_contract_kind` (no code change); graph_builder already skips `provides_contract` / `uses_contract`.

## 2. Extraction

- [x] 2.1 `dynamo_contracts.hpp/.cpp`: JavaScript/TypeScript v3 commands, lib-dynamodb commands, DocumentClient / v2 methods; writes provide, reads use; SDK import required.
- [x] 2.2 Table name from a literal, an env default chain, or a same-file module `const` not shadowed.
- [x] 2.3 One-line hook in `js_extra_walk`; `src/engine/CMakeLists.txt`.
- [x] 2.4 Index version `logic-19`.

## 3. Verification

- [x] 3.1 `dynamo_contracts_test.cpp` (new), `contracts_test` `test_dynamo_contract_facts`, `dedup_test` near-identical DynamoDB names: all fail with the `src` diff reverse-applied and pass after.
- [x] 3.2 Full suite.
- [x] 3.3 Probe: eight graphs against bin-v0.8.0; no node or edge lost or changed except clustering-derived properties; every new fact hand-checked.
- [x] 3.4 Seam discover/fuse and scorer: T34, T35 link; no previously linked edge lost.
