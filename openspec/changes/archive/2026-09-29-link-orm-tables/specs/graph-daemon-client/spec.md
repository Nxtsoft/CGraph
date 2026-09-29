## ADDED Requirements

### Requirement: Impact reaches a file's endpoints and does not walk through service hubs
A `dependents` walk (the `impact` op, and change-context's transitive dependents) SHALL treat an `endpoint` node as a dependent of the file that `contains` it, reached `via` `contains`, when the file is reached by a strong path. A seed starts a strong path. A path turns weak at any step out of a `function`, `class` or `endpoint` node, and every step after a weak one is weak: a changed function or class reaches the endpoints it affects through resolved calls into their handlers. A weakly reached node SHALL still be reported, but a weakly reached file SHALL NOT serve its endpoints. A node reached weakly SHALL be expanded again when a strong path reaches it, so the result does not depend on seed order. Each node SHALL be reported once, from its shallowest state (the strong one on a tie), with that state's own witness: exactly `depth` edges, nearest first, each joining consecutive nodes of the path, ending at the changed seed it names. A served endpoint's witness is therefore the strong path that caused it. Change-context SHALL report each impact's witness from this path. Other things a file contains SHALL NOT become its dependents. A `service` node reached during any walk SHALL be reported but not expanded, so that one edge into a seam's service hub does not reach every endpoint and consumer of that service. A `service` node given as the seed SHALL still be expanded.

#### Scenario: From a table to the endpoint that uses it
- **GIVEN** `model -maps_table-> sql_table:t`, `handler.ts -imports-> model` and `handler.ts -contains-> endpoint:GET /t`
- **WHEN** `impact` runs from `sql_table:t` with `direction: "dependents"`
- **THEN** `model` is at depth 1, `handler.ts` at 2, and `endpoint:GET /t` at 3 `via` `contains`, and a function `handler.ts` contains is not reached

#### Scenario: A function does not reach its file's other routes
- **GIVEN** `handler.ts` containing functions `fmt` and `get_t` and endpoints `GET /t` and `GET /t2`, with `get_t -CALLS-> fmt` and `endpoint:GET /t -handled_by-> get_t`
- **WHEN** `impact` runs from `fmt`
- **THEN** `endpoint:GET /t` is reached through its handler and `endpoint:GET /t2` is not

#### Scenario: A file mounting the route file keeps its routes out
- **GIVEN** `app.ts -imports_from-> handler.ts` and `app.ts -contains-> endpoint:GET /health`
- **WHEN** `impact` runs from `fmt`
- **THEN** `app.ts` is reported and `endpoint:GET /health` is not

#### Scenario: Seed order does not change which endpoints are served, or why
- **GIVEN** the graphs above, with seeds `helper` (a function in `handler.ts`) and `sql_table:t`, in either order
- **THEN** `endpoint:GET /t` is reached at depth 3 with changed seed `sql_table:t` and witness relations `contains`, `imports`, `maps_table`, and every reported node's witness has as many edges as its depth and ends at its changed seed

#### Scenario: An imported function serves no routes
- **GIVEN** `util` declared in `lib.ts` and `handler.ts -imports-> util`
- **WHEN** `impact` runs from `util`
- **THEN** `handler.ts` and `app.ts` are reported and none of their endpoints are

#### Scenario: A service hub is not a bridge
- **GIVEN** `endpoint:GET /t` and `endpoint:GET /other` both `SERVED_BY` `service:api`
- **WHEN** a dependents walk from `sql_table:t` reaches `service:api`
- **THEN** `service:api` is reported and `endpoint:GET /other` is not reached; a walk seeded at `service:api` reaches both endpoints

