## ADDED Requirements

### Requirement: A workspace manifest declares proxy prefixes between members
`cgraph.workspace.json` SHALL accept an optional `prefixes` array of `{"repo", "from", "to"}` objects with absolute paths, normalized (duplicate and trailing slashes dropped). An entry that is malformed, maps a path to itself, or names a repository the manifest does not list SHALL make the manifest unusable, reported in `errors`. Federated `impact` SHALL cross from a provider's endpoint to a member's spelling of it under that member's `from`, and from such a spelling to the provider, only when the member consumes that spelling without serving it; a contract reached only through a member's proxy SHALL NOT be asked back of that member. Federated `path` SHALL join across the proxy and keep both spellings on the path, with `bridged_through` naming the provider's id.

#### Scenario: Impact crosses the proxy both ways
- **GIVEN** a workspace of `idp` and `web` with `{"repo": "web", "from": "/api/backend", "to": "/api"}`, where web's hook consumes `endpoint:PATCH /api/backend/v1/sessions/{}/extend`
- **WHEN** `impact` runs on idp's handler with `dependents`
- **THEN** the hook is reached, tagged `repo: web`, `bridged_through: endpoint:PATCH /api/v1/sessions/{}/extend`; and `impact` from the hook with `dependencies` reaches idp's handler

#### Scenario: A route the member serves itself is not crossed
- **GIVEN** web serves `GET /api/backend/healthz` itself
- **THEN** impact from idp's `GET /api/healthz` handler does not reach web's healthz route

#### Scenario: A prefix naming no member is an error
- **GIVEN** a `prefixes` entry for a repository the manifest does not list, or a relative `from`
- **THEN** the workspace carries errors and no repos
