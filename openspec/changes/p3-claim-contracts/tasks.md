## 1. Extraction

- [x] 1.1 `claim_contracts.cpp`: Kotlin and Java builder chains (jjwt, Nimbus), `apply`/`run` bare claims, registered setters.
- [x] 1.2 Go claims structs: golang-jwt embedding, `ParseWithClaims`, payload-segment `json.Unmarshal`; field-node sources.
- [x] 1.3 TypeScript: library decoders and signers by import, same-file decoder functions, `claim_decoder` / `claim_read` facts.
- [x] 1.4 Kotlin hand-written payload reads; PyJWT / python-jose `jwt.encode` payload keys.
- [x] 1.5 Hook in `extract_with_config`; `graph_builder` skips the intermediate relations.

## 2. Resolution and bridging

- [x] 2.1 `resolve_claim_reads` in step 8 of `resolve_contracts`.
- [x] 2.2 `is_standard_jwt_claim` (IANA registry, issuer-generic names); `is_bridged_contract` false for standard claims.
- [x] 2.3 Index version `logic-18` (after env 15, tables 16, headers 17).

## 3. Verification

- [x] 3.1 `claim_contracts_test.cpp` (fourteen cases, fail before), `contracts_test.cpp` standard claims and table order.
- [x] 3.2 Probe diff on 8 repos, every cross-repo join hand-checked; seam and scorer before/after.

## 4. Review round 1

- [x] 4.1 TS decoders take the claims type only from a library call's type argument, an `as T` on the payload, or a single declared return type of a function that returns the payload.
- [x] 4.2 TS and Kotlin hand-written decoders follow the payload segment through base64 and JSON parsing.
- [x] 4.3 Reads: every parameter and declaration is a binding; only project-local callees and same-file decoders.
- [x] 4.4 One scan per JS/TS file, provider and decoder passes only where a JWT library or `JSON.parse` appears, function spans indexed once, `join_facts` hoisted.
- [x] 4.5 golang-jwt identifier claims argument; library-form positives and negatives in tests.
