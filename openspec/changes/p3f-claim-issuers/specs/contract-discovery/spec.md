## MODIFIED Requirements

### Requirement: One rule says which contract ids cross repositories
`is_bridged_contract(id)` SHALL be true for every `endpoint:` id, for a `claim:` id whose name is not a standard JWT claim (`is_standard_jwt_claim`: a sorted static table of the IANA JSON Web Token Claims registry entries defined by RFC 7519 section 4.1, OpenID Connect Core 1.0, OpenID Connect Front-Channel Logout 1.0, RFC 7800, RFC 8693 and RFC 9449, retrieval date recorded; case-sensitive), for a `header:` id whose name is not a standard HTTP header, and for a `table:` or `label:` id in a named database; it SHALL be false for `table:local:` and `label:local:` ids, for every `env:` id, for a standard claim, for a standard header and for every non-contract id. The standard headers SHALL be a sorted static table of the permanent entries of the IANA HTTP Field Name Registry (lowercased, `*` dropped, retrieval date recorded) plus `x-request-id`, `x-real-ip`, `x-correlation-id`, `traceparent`, `tracestate`, `baggage`, and every `x-forwarded-*` name. `crossing_id` (with the declared databases, env and claim issuers) SHALL give the id a repo's contract crosses at: a member's repo-local table at its database's id; a `claim:<name>` of a member of a declared issuer `I` at `claim:I:<name>`, and nowhere when the name is an RFC 7519 section 4.1 registered claim (`is_registered_jwt_claim`: `iss`, `sub`, `aud`, `exp`, `nbf`, `iat`, `jti`); `claim:I:<name>` itself only for a member of `I`; a declared env id as itself; any other bridged id as itself; else none. `contract_spellings` SHALL give every id a repo may hold a crossing under: for `claim:I:<name>` a member's `claim:<name>` and nothing for any other repo, and nothing for an unscoped `claim:<name>` crossing in a repo declared in an issuer. With no issuer declared, claims SHALL cross exactly as `is_bridged_contract` says. Seam discovery and fuse, workspace `impact` and `path`, and change context's `cross_service` SHALL use these in place of any `endpoint:` prefix test. Proxy prefixes remain endpoint-only.

#### Scenario: Repo-local tables, env names and standard headers do not cross by themselves
- **THEN** `is_bridged_contract` is false for `table:local:users`, `env:NODE_ENV`, `header:authorization`, `header:x-forwarded-for` and `header:traceparent`, and true for `table:turing:orders`, `header:x-tenant-id` and `claim:org_id`

#### Scenario: Declarations make a repo-local table and an env name cross
- **GIVEN** database `turing` of api and ml, and `API_URL` declared for api
- **THEN** `crossing_id` gives ml's `table:local:users` as `table:turing:users`, `env:API_URL` as itself, and nothing for `env:NODE_ENV`; ml's spellings of `table:turing:users` are `table:turing:users` and `table:local:users`, web's only `table:turing:users`

#### Scenario: Standard JWT claims never cross
- **THEN** `is_bridged_contract` is false for `claim:iss`, `claim:sub`, `claim:aud`, `claim:exp`, `claim:nbf`, `claim:iat`, `claim:jti`, `claim:email`, `claim:name`, `claim:preferred_username`, `claim:scope`, `claim:client_id`, `claim:azp`, `claim:nonce` and `claim:sid`, and true for `claim:roles`, `claim:session_id`, `claim:tenant_id`, `claim:permissions` and `claim:EXP`

#### Scenario: A declared issuer scopes claims to its members
- **GIVEN** issuer `idp` of repos idp and web, and api in no issuer
- **THEN** `crossing_id` gives web's `claim:session_id` as `claim:idp:session_id` and idp's `claim:email` and `claim:scope` as `claim:idp:email` and `claim:idp:scope`, nothing for a member's `claim:sub`, `claim:iss`, `claim:aud`, `claim:exp`, `claim:nbf`, `claim:iat` or `claim:jti`, api's `claim:session_id` as itself, and nothing for api's `claim:email` or `claim:idp:session_id`
- **AND** web's spellings of `claim:idp:email` are `claim:email`, api's are none, web's of `claim:session_id` are none and api's are `claim:session_id`

#### Scenario: No issuer, no change
- **GIVEN** no issuer declared
- **THEN** `crossing_id` gives `claim:session_id` as itself and nothing for `claim:email`
