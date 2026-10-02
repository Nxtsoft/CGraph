## ADDED Requirements

### Requirement: Change context crosses at every contract kind
Change context SHALL track every contract (`contract_kind_of` non-empty) the change touches as it tracks endpoints: one the change edits, removes or adds, one whose provider it reaches (`handled_by`) or that a changed file contains serves it; `CONSUMES` from changed code, or from a direct caller of a changed helper, uses it. `cross_service` SHALL ask a repo-local table or label only of the members declared in the home repo's database, under their own `table:local:` spelling, with `shared_id` naming the database's id; one with no declared database SHALL be listed with `local: true` and asked of nobody. A used env variable with a declared provider other than the home repo SHALL yield one `provider` row of kind `service` for that member. Contract nodes SHALL never be rows.

#### Scenario: Header provider and user are touched
- **GIVEN** `readTenant` providing `header:X-Tenant-Id` and `sendTenant` using `header:x-org-id` and `env:ML_URL`
- **THEN** a change to `readTenant` serves `header:x-tenant-id` at rank 1, a change to `sendTenant` uses `header:x-org-id` at rank 1 and `env:ML_URL`

#### Scenario: A repo-local table is asked inside its database only
- **GIVEN** home api serving `table:local:users`, members ml (database `turing` with api), web, and billing (no database), and `env: [{"name": "ML_URL", "service": "ml"}]`
- **THEN** ml is asked for `table:local:users` and its `listUsers` is a consumer row, billing is never asked for a table, web's sender of `header:x-tenant-id` is a row while the header node itself is not, and `env:ML_URL` has a provider row for ml of kind `service`; with billing as home, `table:local:users` is listed `local` and nobody is asked
