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

## 4. Review round 1 (PR #168)

- [x] 4.1 Caveat in README, `dynamo_contracts.hpp` and `contracts.hpp`: a `dynamo:` name joins every supplied repo whatever AWS account or region; account declaration recorded as a follow-up; README says Python boto3 is not read.
- [x] 4.2 Seam discover copies `env` onto the joined node (`seam_test` `test_dynamo_contracts`, which also joins `dynamo:` across two repo graphs and fuses it).
- [x] 4.3 Kind lists in `workspace.hpp`, `seam.hpp`, `change_context.hpp`, `contracts.hpp` name DynamoDB tables.
- [x] 4.4 `contracts_test`: the standard-claim comment back above its function.
- [x] 4.5 `DynamoContractsFileScope`: the file's imports and constants are read once per file.
- [x] 4.6 `paginateQuery` / `paginateScan` (TableName in the second argument), namespaced commands (`new ddb.PutItemCommand`), destructured parameters and locals shadow a module constant; tests for each fail on the round-0 code.

## 5. Merge with main (#169 claim issuers)

- [x] 5.1 `git merge origin/main` (8be77de): `contracts.hpp` keeps both the issuer and the DynamoDB notes; `seam.cpp` keeps `env` in the copied keys and #169's issuer tagging; `seam_test` keeps `test_dynamo_contracts` and `test_issuer_claims`; the MODIFIED "One rule says which contract ids cross repositories" block is #169's text plus `dynamo:`.
- [x] 5.2 Index version `logic-21`.
