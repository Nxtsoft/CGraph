## ADDED Requirements

### Requirement: Seam discovery and fuse join a consumer through its declared proxy prefix
`seam discover` and `seam fuse` SHALL accept `--prefix REPO:/from=/to`. With a prefix, an endpoint that graph `REPO` consumes but neither serves nor documents, whose path is `from` or lies beneath it on a segment boundary, SHALL be joined as the same method at `to` + the rest, unless only `REPO` itself serves that proxied endpoint. The joined endpoint's `CONSUMED_AT` edges SHALL carry `via` = the consumer's own path, and the resolution log SHALL count the endpoints each prefix joined. `seam fuse` SHALL redirect such a consumer's `CONSUMES` edge to the joined endpoint, SHALL NOT render the placeholder it leaves unused, and SHALL fail when the joined endpoint is in neither the seam nor a service graph. A prefix naming a repository no `--graph` provides SHALL be refused (exit 2). Without a prefix nothing is joined across spellings, and no graph's own ids change.

#### Scenario: A proxied call meets the backend
- **GIVEN** `web` consuming `endpoint:PATCH /api/backend/v1/sessions/{}/extend` and `idp` serving `endpoint:PATCH /api/v1/sessions/{}/extend`
- **WHEN** `seam discover` runs with `--prefix web:/api/backend=/api`
- **THEN** `web`'s caller is `CONSUMED_AT` idp's endpoint with `via: /api/backend/v1/sessions/{}/extend`, and the log reads `prefix web /api/backend -> /api: 1 consumed endpoints joined at the proxied path`

#### Scenario: A route the consumer serves itself is not proxied
- **GIVEN** `web` also serves `GET /api/backend/healthz`, and serves `GET /api/saml/metadata`
- **THEN** its call to `/api/backend/healthz` stays on its own endpoint, and its call to `/api/backend/saml/metadata` is not joined to `/api/saml/metadata`

#### Scenario: Fuse refuses an unjoined proxied edge
- **GIVEN** a seam discovered without the provider graph
- **WHEN** fused with the same prefix
- **THEN** fuse fails and names the proxied endpoint
