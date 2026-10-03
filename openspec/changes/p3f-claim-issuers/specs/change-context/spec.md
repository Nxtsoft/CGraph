## ADDED Requirements

### Requirement: Change context asks about a claim only within its declared issuer
When the home repository is declared in a claim issuer, change context's `cross_service` SHALL treat its touched claims by the same `crossing_id` and `contract_spellings` as workspace `impact`: a claim crossing at `claim:<issuer>:<name>` SHALL be listed with that `shared_id` and asked only of the issuer's other members, under their own `claim:<name>`; a registered RFC 7519 claim SHALL be listed `local: true` and asked of nobody.

#### Scenario: A token builder's change reaches its issuer's readers only
- **GIVEN** home idp and web declared in issuer `idp`, api in none, and a change to idp code serving `claim:email`, `claim:session_id` and `claim:sub`, every repo holding a reader of each
- **THEN** the rows are exactly web's `email` and `session_id` readers, api is never asked, `claim:email` is listed with `shared_id: claim:idp:email`, and `claim:sub` is listed `local`
