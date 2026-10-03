## ADDED Requirements

### Requirement: A workspace manifest declares claim issuers
`cgraph.workspace.json` SHALL accept an optional `issuers` array of `{"name", "repos": [...]}`, round-tripped by `workspace_manifest_json` and validated as `databases` are: a name with `:` or whitespace, the name `local`, an empty, non-array or non-string `repos`, a repo the manifest does not list, a repo in two issuers, or an issuer declared twice SHALL make the manifest unusable, reported in `errors`, never an exception. `impact` and `path` SHALL cross at a member's claim only towards the other members of its issuer, at `crossing_id`'s `claim:<issuer>:<name>`, asking each member under its own `claim:<name>`; a registered RFC 7519 claim SHALL not cross, and a repo outside the issuer SHALL never be asked about it.

#### Scenario: Impact crosses within the issuer only
- **GIVEN** repos idp, web and api, `"issuers": [{"name": "idp", "repos": ["idp", "web"]}]`, idp's `mintToken` reaching `claim:session_id`, `claim:email` and `claim:sub`, and web and api each holding a reader of all three
- **THEN** impact from `mintToken` reaches exactly web's `session_id` reader through `claim:idp:session_id` and web's `email` reader through `claim:idp:email`
- **AND** with no `issuers`, it reaches exactly web's and api's `session_id` readers through `claim:session_id`

#### Scenario: A bad issuer is an error
- **GIVEN** an issuer naming a repo the manifest does not list, a repo in two issuers, a name declared twice, the name `local`, a name with `:`, a non-string repo, an empty `repos`, or `issuers` that is not an array
- **THEN** `load_workspace` returns errors and no repos, and does not throw
