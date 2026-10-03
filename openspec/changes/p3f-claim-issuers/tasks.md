## 1. Declarations

- [x] 1.1 `ClaimIssuer`, `parse_claim_issuers`, `parse_issuer_flag`, `claim_issuers_json`, `declared_issuer`, `issuer_claim`; database and issuer parsing and validation share one implementation.
- [x] 1.2 `contract_declaration_errors` validates issuers (unknown repo, repo in two issuers, name twice).
- [x] 1.3 `is_registered_jwt_claim` (RFC 7519 section 4.1).

## 2. One crossing rule

- [x] 2.1 `crossing_id` / `contract_spellings` take issuers: members cross at `claim:<issuer>:<name>` except registered names; outsiders never meet members; no issuer, no change.
- [x] 2.2 Seam discover/fuse (`issuer` property, log line, `--issuer` in the refusal), CLI `--issuer` on both.
- [x] 2.3 Workspace manifest `issuers` (load, validate, round trip); impact/path and change context pass issuers.
- [x] 2.4 Index version `logic-20`; README.

## 3. Verification

- [x] 3.1 Tests in contract_declarations, contracts, seam, workspace and change_context; the workspace and change context ones fail on origin/main.
- [x] 3.2 Probe: seam with `--issuer idp=idp,idp-front-end,passless-cli,passless-app`, every cross-repo claim join before/after hand-checked; seam without `--issuer` byte-identical to bin-v0.8.0's; scorer before/after.
