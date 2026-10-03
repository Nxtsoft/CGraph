# Claims cross repositories within a declared JWT issuer (Phase 3 follow-up)

## Why

A claim name alone does not say whose token it sits in. Since Phase 3.3 (#164) an application claim (`session_id`, `roles`, `tenant_id`) joins any two repositories that name it, even when their tokens come from different issuers, and a standard claim (the 45 IANA registry names, OpenID Connect's `email`, `name`, `scope` included) joins none, even between an identity provider and the clients reading its ID token. The #164 proposal recorded the follow-up: declare which repositories share an issuer, as databases are declared.

## What Changes

- **Declarations.** `cgraph.workspace.json` takes `issuers: [{"name", "repos": [...]}]`; `seam discover` and `seam fuse` take `--issuer NAME=repoA,repoB`. Validated exactly like `databases`: a name with `:` or whitespace, the reserved name `local`, an empty or non-string `repos`, an unknown repo, a repo in two issuers, or an issuer declared twice is a manifest error (seam commands exit 2), never an exception. A repo may be in a database and an issuer.
- **The crossing rule** (`crossing_id` / `contract_spellings`, contract_declarations.cpp), so seam discover/fuse, workspace `impact`/`path` and change context's `cross_service` agree:
  - A member of issuer `I` crosses its `claim:<name>` at `claim:I:<name>` for every name except the RFC 7519 section 4.1 registered claims (`iss`, `sub`, `aud`, `exp`, `nbf`, `iat`, `jti`, `is_registered_jwt_claim`), so OIDC claims join between members.
  - A member holds a `claim:I:<name>` crossing under its own `claim:<name>`; a repo outside `I` holds it under nothing and never crosses there; a member holds no unscoped `claim:<name>` crossing. So a member's claims never join a repo outside its issuer.
  - A repo in no issuer keeps the #164 rule, and with no issuer declared nothing changes.
- **Seam.** Discover sets the joined node's `issuer` property and logs `issuer I (a,b): N claims joined at the issuer's id` per declared issuer (no line when none is declared); fuse renames a member's claim to the issuer's id with the `issuer` property and refuses a seam joined under an issuer it is not given, naming `--issuer`.
- Index version `logic-20`.

## Design

- Mirrors database scoping: a single-repo graph never depends on a workspace (claim ids stay `claim:<name>`), and the issuer's spelling exists only where repos are matched. Passing issuers into extraction would make a graph depend on its workspace.
- Only the RFC 7519 registered names stay home inside an issuer: they describe the token itself (who issued it, for whom, when it expires), not a value one service hands another. The OIDC and RFC 8693 names (`email`, `name`, `scope`, `client_id`) are claims the issuer writes for its clients to read, which is exactly the join a declaration asserts.
- An issuer's id `claim:I:<name>` is recognised only for a declared issuer name, so an undeclared namespaced claim (`https://example.com/roles`) is untouched.

## Contract that tests verify

- `contract_declarations_test.cpp` `test_issuer_crossing`, `test_issuer_parse_and_validate`: the crossing and spelling rules for members, outsiders and registered names, unchanged behaviour with no issuer, parsing, flags, round trip, every malformed or conflicting issuer an error.
- `contracts_test.cpp`: `is_registered_jwt_claim` is exactly the seven RFC 7519 names, each also standard.
- `seam_test.cpp` `test_issuer_claims`: undeclared baseline; with `idp=idp,web,cli` members join at `claim:idp:*` (OIDC `email` included, `sub` never), api's claims stay apart, the log line, fuse refuses without `--issuer` and shares the issuer's ids with it.
- `workspace_test.cpp` `test_impact_crosses_within_a_declared_issuer`: impact crosses to members only, through `session_id` and `email` and never `sub`; undeclared it crosses `session_id` to every repo; eight bad manifests are errors.
- `change_context_test.cpp` `test_issuer_crossings`: `cross_service` asks members only, lists the `shared_id`, and lists `sub` local.

## Non-goals

- Inferring issuers from code (token signing keys, JWKS URLs).
- Changing which claims are standard outside an issuer.
- Seeding `impact` at an issuer's spelling, which no single graph holds (as for databases).

## Impact

- `src/engine/contract_declarations.cpp`, `contract_declarations.hpp`, `contracts.cpp`, `contracts.hpp`.
- `src/engine/seam.cpp`, `seam.hpp`, `src/cli/main.cpp` (`--issuer`).
- `src/engine/workspace.cpp`, `workspace.hpp`, `src/engine/change_context.cpp`.
- `src/engine/index_persistence.cpp` (`logic-20`), README.
- Output: seam fragments and fused graphs change only when `--issuer` is given; graphs are unchanged.
