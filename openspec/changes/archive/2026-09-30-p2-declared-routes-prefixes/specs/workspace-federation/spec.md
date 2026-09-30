## ADDED Requirements

### Requirement: A workspace manifest declares proxy prefixes between members
`cgraph.workspace.json` SHALL accept an optional `prefixes` array of `{"repo", "from", "to"}` objects with absolute paths, normalized (duplicate and trailing slashes dropped). An entry that is malformed, maps a path to itself, or names a repository the manifest does not list SHALL make the manifest unusable, reported in `errors`. Federated `impact` SHALL cross from a provider's endpoint to a member's spelling of it under that member's `from`, and from such a spelling to the provider, only when the member consumes that spelling without serving it; a contract reached only through a member's proxy SHALL NOT be asked back of that member. A member's proxied spelling SHALL NOT be crossed at an endpoint only that member serves, nor at a contract the traversal reached only inside that member. A `prefixes` entry, `repos` entry or manifest `name` whose members are not strings SHALL be a manifest error, never an exception. Federated `path` SHALL join across the proxy and keep both spellings on the path, with `bridged_through` naming the provider's id.

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

#### Scenario: A proxied call does not join a third member at the caller's own route
- **GIVEN** members idp, web and mobile, prefix `web:/api/backend=/api`, web alone serving `GET /api/saml/metadata`, web calling `/api/backend/saml/metadata`, and mobile calling `GET /api/saml/metadata`
- **THEN** `impact` from web's caller (either direction) and from `endpoint:GET /api/backend/saml/metadata` does not reach mobile, and `path` from web's caller to mobile's is empty; once idp serves the path too, all three cross

#### Scenario: A member's own route does not reach its proxied callers
- **GIVEN** web serves `GET /api/saml/metadata` and its `getIdpMetadata` calls `/api/backend/saml/metadata` through the prefix
- **WHEN** `impact` runs on web's route handler with `dependents`
- **THEN** `getIdpMetadata` is not reached, whether or not idp also serves the path; when idp serves it, `impact` on idp's handler reaches `getIdpMetadata`

#### Scenario: A non-string manifest member is an error
- **GIVEN** a `prefixes` entry `{"repo": 7}`, a `repos` entry `{"name": 5}`, or `"name": 3`
- **THEN** `load_workspace` returns errors and no repos, and does not throw
