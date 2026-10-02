## ADDED Requirements

### Requirement: JWT claim facts come only from provably-JWT code
Extraction SHALL emit a `claim:<name>` fact only from code that is provably about a JSON Web Token, never from a claim-like name alone (`session_id`, `roles`, `email` are ordinary field and map-key names). Providers (`provides_contract`, source the enclosing function): `.claim("x", v)` with a literal name on a call chain rooted at jjwt's `Jwts.builder()` or Nimbus's `JWTClaimsSet.Builder()` (Kotlin and Java), a bare `claim("x", v)` inside an `apply { }` / `run { }` lambda on such a chain (not inside a nested `apply` / `run` / `with`), the registered-claim setters on such a chain (`subject` / `setSubject` -> `sub`, `issuer` -> `iss`, `audience` -> `aud`, `expiration` / `expirationTime` -> `exp`, `notBefore` / `notBeforeTime` -> `nbf`, `issuedAt` / `issueTime` -> `iat`, `id` / `jwtID` -> `jti`), the literal keys of the payload object of `sign({..})` from `jsonwebtoken` and `new SignJWT({..})` from `jose` plus `SignJWT`'s setters, and the string keys of the dict passed to `jwt.encode({..})` where `jwt` is imported as PyJWT's or python-jose's module. Users (`uses_contract`): the `json:"x"` tags (not `-`) of a Go struct that embeds golang-jwt's `RegisteredClaims` / `StandardClaims`, is the composite-literal claims argument of that package's `ParseWithClaims`, or is the `json.Unmarshal` target of bytes derived from segment `[1]` of a `strings.Split(token, ".")` in a file that base64-decodes (source the field node); the properties of a same-file TypeScript interface or object type alias that `jwtDecode<T>` (`jwt-decode`), `decodeJwt<T>` / `jwtVerify<T>` (`jose`) or a `jsonwebtoken` `verify` / `decode` cast `as T` types the payload as, or that a function's declared return type names when its body calls one of those or decodes the payload by hand (`.split('.')`, a `[1]` subscript, `JSON.parse` and `'base64'` / `'base64url'` / `atob`) (source the field node); a property read `v.x` in any file where `v` holds a call to such a decoder function (through `await`, `?:`, `??`, `||` and identifier aliases in enclosing scopes), the callee resolves through the reading file's imports or its own declarations to the decoder, and the decoder's type declares `x` (source the reading function); and Kotlin `.jsonObject["x"]` directly on `Json.parseToJsonElement(..)` in a function that also splits on `'.'`, takes segment 1 (`getOrNull(1)`, `get(1)`, `[1]`) and calls a `Base64...decode(..)` (source the function). The intermediate `claim_decoder` / `claim_read` relations SHALL never become code-graph edges.

#### Scenario: A jjwt builder writes its claims
- **GIVEN** `Jwts.builder().subject(u).expiration(e).claim("roles", r).claim("tenant_id", t).apply { sessionId?.let { claim("session_id", it) }; bindingClaims.forEach { (k, v) -> claim(k, v) }; kid?.let { header().add("kid", it) } }` in `generateAccessToken`, and `builder.claim("not_a_jwt_claim", 1)` on another object
- **THEN** `generateAccessToken` provides exactly `sub`, `exp`, `roles`, `tenant_id` and `session_id`, and no other claim is minted

#### Scenario: A Go struct decoded from the payload segment is a claims type
- **GIVEN** `Decode` splitting a token on ".", unmarshalling `decodeSegment(parts[0])` into `t.Header` and `decodeSegment(parts[1])` into `t.Claims`, a `decodeSegment` that calls `base64.RawURLEncoding.DecodeString`, and a plain `UserInfo` struct with `json:"roles"`
- **THEN** the `Claims` fields' tags (`sub`, `tenant_id`, `session_id`) are used claims and neither `alg` nor `roles` is a claim

#### Scenario: A TypeScript read through an imported decoder
- **GIVEN** `decodeToken(token): DecodedToken | null` that splits on '.', takes `parts[1]`, base64-decodes and `JSON.parse`s, and `proxy` in another file doing `const decoded = decodeToken(t); let effectiveDecoded = decoded; ... effectiveDecoded.roles` and `effectiveDecoded.notAClaim`
- **THEN** `proxy` consumes `claim:roles` and nothing else, `DecodedToken`'s properties are used claims, and an untyped `decodeJwt(token): any` and a header decoder reading `parts[0]` yield none

#### Scenario: A Kotlin hand-written payload read
- **GIVEN** `sessionIdClaim` doing `accessToken.split('.').getOrNull(1)`, `Base64.UrlSafe...decode(payload)` and `Json.parseToJsonElement(..).jsonObject["session_id"]`, and `bodyRoles` indexing `jsonObject["roles"]` on a response body
- **THEN** `sessionIdClaim` uses `claim:session_id` and `roles` is not a claim

## MODIFIED Requirements

### Requirement: One rule says which contract ids cross repositories
`is_bridged_contract(id)` SHALL be true for every `endpoint:` id, for a `claim:` id whose name is not an RFC 7519 section 4.1 registered claim (`iss`, `sub`, `aud`, `exp`, `nbf`, `iat`, `jti`, case-sensitive, `is_registered_jwt_claim`), for a `header:` id whose name is not a standard HTTP header, and for a `table:` or `label:` id in a named database; it SHALL be false for `table:local:` and `label:local:` ids, for every `env:` id, for a registered claim, for a standard header and for every non-contract id. The standard headers SHALL be a sorted static table of the permanent entries of the IANA HTTP Field Name Registry (lowercased, `*` dropped, retrieval date recorded) plus `x-request-id`, `x-real-ip`, `x-correlation-id`, `traceparent`, `tracestate`, `baggage`, and every `x-forwarded-*` name. `crossing_id` (with the declared databases and env) SHALL give the id a repo's contract crosses at: a member's repo-local table at its database's id, a declared env id as itself, a bridged id as itself, else none; `contract_spellings` SHALL give every id a repo may hold a crossing under. Seam discovery and fuse, workspace `impact` and `path`, and change context's `cross_service` SHALL use these in place of any `endpoint:` prefix test. Proxy prefixes remain endpoint-only.

#### Scenario: Repo-local tables, env names and standard headers do not cross by themselves
- **THEN** `is_bridged_contract` is false for `table:local:users`, `env:NODE_ENV`, `header:authorization`, `header:x-forwarded-for` and `header:traceparent`, and true for `table:turing:orders`, `header:x-tenant-id` and `claim:org_id`

#### Scenario: Declarations make a repo-local table and an env name cross
- **GIVEN** database `turing` of api and ml, and `API_URL` declared for api
- **THEN** `crossing_id` gives ml's `table:local:users` as `table:turing:users`, `env:API_URL` as itself, and nothing for `env:NODE_ENV`; ml's spellings of `table:turing:users` are `table:turing:users` and `table:local:users`, web's only `table:turing:users`

#### Scenario: Registered JWT claims never cross
- **THEN** `is_bridged_contract` is false for `claim:iss`, `claim:sub`, `claim:aud`, `claim:exp`, `claim:nbf`, `claim:iat` and `claim:jti`, and true for `claim:roles`, `claim:session_id` and `claim:EXP`
