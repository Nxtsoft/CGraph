## ADDED Requirements

### Requirement: Seam discovery and fuse join claims within a declared issuer
`seam discover` and `seam fuse` SHALL accept `--issuer NAME=repoA,repoB` (a workspace manifest's `issuers`), validated as `--database` is: a malformed value, the name `local`, a repo that is not among the `--graph`s, a repo in two issuers, or a name declared twice SHALL exit 2. Discover SHALL join a member's claim at the issuer's id `crossing_id` gives (`claim:<NAME>:<name>`), with the node's `issuer` property set to the issuer and its label the claim's own name, and SHALL leave out a member's RFC 7519 registered claims; a repo outside the issuer SHALL keep the undeclared rule and never join the members' claims. Discover SHALL log `issuer NAME (repoA,repoB): N claims joined at the issuer's id` for each declared issuer and no such line when none is declared, so that with no `--issuer` the seam and its log are unchanged. Fuse SHALL rename a member's claim to the issuer's id with the `issuer` property and keep every other claim's existing treatment; fused without the `--issuer` discover was given, a seam claim SHALL fail loud naming the contract and `--issuer`.

#### Scenario: Members of an issuer join, outsiders do not
- **GIVEN** idp providing `claim:session_id`, `claim:email`, `claim:sub` and `claim:roles`; web using `claim:session_id`, `claim:email` and `claim:sub`; cli using `claim:email`; api using `claim:session_id`, `claim:roles` and `claim:email`
- **WHEN** discover runs with no declaration
- **THEN** `claim:session_id` is `HANDLED_BY` idp's `mintToken` and `CONSUMED_AT` api's `checkToken`, and no `claim:email` node is in the seam
- **WHEN** discover runs with `--issuer idp=idp,web,cli`
- **THEN** `claim:idp:email` (kind `claim`, label `email`, `issuer: idp`) is `HANDLED_BY` idp's `mintToken` and `CONSUMED_AT` web's `readToken` and cli's `whoami`; `claim:idp:session_id` is `CONSUMED_AT` web's `readToken` and not api's; no `claim:sub`, `claim:idp:sub` or `claim:email` node exists; api's `claim:session_id` has no provider and no `issuer`; the log says `issuer idp (idp,web,cli): 6 claims joined at the issuer's id`

#### Scenario: Fuse shares the issuer's ids and refuses without them
- **GIVEN** that seam
- **WHEN** fuse runs without `--issuer`
- **THEN** it fails naming `--issuer`
- **WHEN** fuse runs with `--issuer idp=idp,web,cli`
- **THEN** web's `readToken` and cli's `whoami` have `CONSUMES` edges to `claim:idp:email` (`issuer: idp`), `claim:idp:email` is `handled_by` idp's `mintToken`, api's `checkToken` consumes `claim:session_id` and `api::claim:email`, and `web::claim:sub` stays scoped
