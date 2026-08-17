## ADDED Requirements

### Requirement: Enrichment nodes rank below structural results in query search
The `graph_query` search route SHALL rank semantic-enrichment nodes after all structural code
nodes. Enrichment nodes are those in the host-authored `doc:`, `concept:`, `media:`, and `topic:`
id namespaces. They SHALL still appear in the results and SHALL be counted in `total`; only their
ordering changes. An exact symbol-name match SHALL still route to the entity result and always be
returned.

#### Scenario: A concept mentioning a symbol does not out-rank the code
- **GIVEN** two functions and a `concept:` node whose label mentions the same token, all matching a non-exact query
- **WHEN** the query runs
- **THEN** the route is `search`, `total` counts all three, and the two functions are returned before the concept

#### Scenario: An exact symbol match still returns the code
- **GIVEN** a function `resolveRepoId` and a concept whose label mentions `resolveRepoId`
- **WHEN** the exact query `resolveRepoId` runs
- **THEN** the route is `entity` and the function is returned
