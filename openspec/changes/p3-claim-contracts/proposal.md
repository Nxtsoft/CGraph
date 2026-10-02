# JWT claims as contracts, typed only (Phase 3.3)

## Why

The foundation (p3-contract-foundation) mints `claim:<name>` nodes from `provides_contract` / `uses_contract` facts, but no extractor emits them, so the probe's claim edges (M30, M31, M32) do not link. Claim names alone are useless evidence: `session_id` appears in 7 of 8 probe repos (98 hits) and `.roles` 409 times in 3; passless-cli's `UserInfo` has a non-JWT `Roles []string json:"roles"` and idp reads Neo4j `record["roles"]`. A claim fact may only come from code that is provably about a JWT.

## What Changes

- **New `claim_contracts.cpp`**, run once per parsed file after the symbol walk (`extract_with_config`), emitting claim facts only from typed JWT contexts:
  - Providers: jjwt `Jwts.builder()` and Nimbus `JWTClaimsSet.Builder()` chains in Kotlin and Java (`.claim("x", v)`, bare `claim("x", v)` in the chain's `apply { }` / `run { }`, registered setters `subject` -> `sub` and so on), `jsonwebtoken` `sign({..})` and jose `new SignJWT({..})` payload keys plus `SignJWT` setters, PyJWT / python-jose `jwt.encode({..})` dict keys.
  - Users: Go structs embedding golang-jwt `RegisteredClaims` / `StandardClaims`, passed to `ParseWithClaims`, or `json.Unmarshal`ed from bytes derived from segment `[1]` of a `strings.Split(token, ".")` in a file that base64-decodes (header structs from `[0]` are excluded); TypeScript interfaces / object type aliases typed by `jwtDecode<T>`, `decodeJwt<T>`, `jwtVerify<T>`, `jsonwebtoken` `verify` / `decode` `as T`, or named by the return type of a function that calls one of those or splits, takes `[1]`, base64-decodes and `JSON.parse`s; property reads `v.x` of a variable holding such a decoder's result in any file (`claim_decoder` / `claim_read` facts, resolved through imports in `resolve_contracts`, kept only when the decoder's type declares `x`); Kotlin `.jsonObject["x"]` on `Json.parseToJsonElement(..)` in a function that splits on '.', takes segment 1 and calls `Base64...decode`.
- **Registered claims never bridge.** `is_registered_jwt_claim` (`iss`, `sub`, `aud`, `exp`, `nbf`, `iat`, `jti`) and `is_bridged_contract` false for them, like standard headers: every token carries them, so two repos naming `exp` are no evidence of a shared token. They are still minted in each repo's graph.
- Hooks: one call in `extractor.cpp`, the two intermediate relations on `graph_builder`'s skip list, step 8 of `resolve_contracts` reads the resolved reads beside the extracted facts. Index version `logic-15`.

## Design choices

- **Typed only, no bare names.** Every rule needs a library call, a builder chain root, or a hand-written payload decode (split on '.', the payload segment, base64, JSON) in the same function or file; the probe's non-JWT `roles` / `session_id` fields stay out.
- **Cross-file reads are resolved, not guessed.** A read is emitted only for a callee the file imports or declares, and becomes a fact only when the callee resolves to a decoder whose returned type declares that property (`effectiveDecoded.notAClaim` is dropped).
- **Untyped decoders are skipped**: idp-front-end's route-local `decodeJwt(token): any` reads (`jwtPayload?.roles`) are not claims.
- **Source anchors**: a typed field's own `field` node; otherwise the innermost function (the file at module level).

## Evidence

- Fail before: `claim_contracts_test` does not compile on origin/main (new header); with that include removed all nine tests fail at runtime (`jjwt builder claims: { }` ... `claim ids across repos`). `contracts_test` fails `a registered claim bridges: claim:iss`; `index_persistence_test` fails (`logic-15`).
- Suite: 89 of 90 pass; `cgraph_file_watcher_test` fails as on main.
- Probe (8 repos, bin-v0.7.3 vs this branch): no node or edge lost or changed in any repo (layout and community properties excluded); new nodes are claim contracts only, new edges only `handled_by` / `contains` / `CONSUMES` on them. 16 provider-to-consumer claim joins, all hand-checked correct (idp to idp-front-end 12, to passless-cli 3, to passless-app 1); no Turing join.
- Scorer: ModSquad 26/40 to 29/40 (M30, M31, M32 linked); Turing 25/41 unchanged.
- Cost (Debug builds of origin/main and this branch, same host, three runs each): turing-webapp extract 2353/2388/2833 ms to 2730/2939/3395 ms, total 10.1/10.2/11.3 s to 10.6/11.8/12.4 s; idp-front-end total 4.1/4.4/5.1 s to 4.2/4.8/5.0 s.

## Non-goals

- T40 (an env name shaped as an Auth0 audience) belongs to the env track.
- Python consumers (`jwt.decode(..)["x"]`), Java/Kotlin jjwt parser reads (`claims.get("x")`), builders held in variables, `.claims(mapOf(..))`.
- Untyped decoders (`: any`) and claims read off them.

## Impact

`src/engine/claim_contracts.cpp` (+ header), hooks in `extractor.cpp`, `graph_builder.cpp`, `contracts.cpp`; `contracts.hpp` gains `is_registered_jwt_claim`; tests `claim_contracts_test.cpp`, `contracts_test.cpp`, `index_persistence_test.cpp`; README contract section.
