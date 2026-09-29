## ADDED Requirements

### Requirement: Impact reaches a file's endpoints and does not walk through hubs
A `dependents` walk (the `impact` op, and change-context's transitive dependents) SHALL treat an `endpoint` node as a dependent of the file that `contains` it, so a walk that reaches a handler file continues to the endpoints that file serves, reached `via` `contains`. Other things a file contains SHALL NOT become its dependents. A `service` node reached during any walk SHALL be reported but not expanded, so that one edge into a seam's service hub does not reach every endpoint and consumer of that service. A `service` node given as the seed SHALL still be expanded. A file reached by climbing `contains` from a symbol it declares SHALL be reported but not expanded, because the file's importers that use the symbol reach it by name; a file given as the seed SHALL still be expanded.

#### Scenario: From a table to the endpoint that uses it
- **GIVEN** `model -maps_table-> sql_table:t`, `handler.ts -imports-> model` and `handler.ts -contains-> endpoint:GET /t`
- **WHEN** `impact` runs from `sql_table:t` with `direction: "dependents"`
- **THEN** `model` is at depth 1, `handler.ts` at 2, and `endpoint:GET /t` at 3 `via` `contains`, and a function `handler.ts` contains is not reached

#### Scenario: A service hub is not a bridge
- **GIVEN** `endpoint:GET /t` and `endpoint:GET /other` both `SERVED_BY` `service:api`
- **WHEN** a dependents walk from `sql_table:t` reaches `service:api`
- **THEN** `service:api` is reported and `endpoint:GET /other` is not reached; a walk seeded at `service:api` reaches both endpoints

#### Scenario: A symbol's file does not lead to its barrel's importers
- **GIVEN** `schema.ts -contains-> model`, `barrel.ts -re_exports-> schema.ts` and `unrelated.ts -imports_from-> barrel.ts`
- **WHEN** a dependents walk from `sql_table:t` reaches `schema.ts` at depth 2
- **THEN** `schema.ts` is reported and neither `barrel.ts` nor `unrelated.ts` is reached; a walk seeded at `schema.ts` reaches `unrelated.ts`
