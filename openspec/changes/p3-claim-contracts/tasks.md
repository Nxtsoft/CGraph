## 1. Extraction

- [x] 1.1 `claim_contracts.cpp`: Kotlin and Java builder chains (jjwt, Nimbus), `apply`/`run` bare claims, registered setters.
- [x] 1.2 Go claims structs: golang-jwt embedding, `ParseWithClaims`, payload-segment `json.Unmarshal`; field-node sources.
- [x] 1.3 TypeScript: library decoders and signers by import, same-file decoder functions, `claim_decoder` / `claim_read` facts.
- [x] 1.4 Kotlin hand-written payload reads; PyJWT / python-jose `jwt.encode` payload keys.
- [x] 1.5 Hook in `extract_with_config`; `graph_builder` skips the intermediate relations.

## 2. Resolution and bridging

- [x] 2.1 `resolve_claim_reads` in step 8 of `resolve_contracts`.
- [x] 2.2 `is_registered_jwt_claim`; `is_bridged_contract` false for registered claims.
- [x] 2.3 Index version `logic-15`.

## 3. Verification

- [x] 3.1 `claim_contracts_test.cpp` (nine cases, fail before), `contracts_test.cpp` registered claims.
- [x] 3.2 Probe diff on 8 repos, every cross-repo join hand-checked; seam and scorer before/after.
