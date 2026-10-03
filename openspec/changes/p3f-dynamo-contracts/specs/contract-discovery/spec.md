## ADDED Requirements

### Requirement: DynamoDB tables a service writes and reads become dynamo contracts
The JavaScript/TypeScript extractor SHALL record `provides_contract` with context `dynamo:<name>` where code writes a DynamoDB table and `uses_contract` with context `dynamo:<name>` where code only reads one, from the innermost enclosing function (else `js_syntax::reading_scope_id`). Writes are `new` of `PutItemCommand`, `UpdateItemCommand`, `DeleteItemCommand`, `CreateTableCommand`, `PutCommand`, `UpdateCommand`, `DeleteCommand` and calls of a method `put`, `update`, `delete`, `putItem`, `updateItem`, `deleteItem`, `createTable`; reads are `GetItemCommand`, `QueryCommand`, `ScanCommand`, `GetCommand` and methods `get`, `query`, `scan`, `getItem`. A call SHALL count only when its first argument is an object literal with a `TableName` property and its file imports `@aws-sdk/client-dynamodb`, `@aws-sdk/lib-dynamodb`, `aws-sdk/clients/dynamodb` or `aws-sdk` (static `import`, `require`, or dynamic `import()`). The name SHALL be read from `TableName` as a string literal (a template with no substitution), as the literal default of a `||` / `??` chain whose other operands are all `process.env.X` / `process.env['X']` reads, recording those variables in order as the fact's `target_label`, or as an identifier naming a module-level `const` of the same file with such a value when no parameter or local of an enclosing scope shadows it. Anything else (`this.tableName`, a parameter, a non-env fallback, an env read with no default, an interpolated template, an imported constant, a batch or transaction request) and a name outside `[A-Za-z0-9_.-]{3,255}` SHALL record nothing, and a test source (`is_test_source_path`) SHALL record nothing. Item keys SHALL NOT be read.

#### Scenario: A writer provides, a reader uses, through an env default
- **GIVEN** `const TABLE_NAME = process.env.DYNAMODB_TABLE_NAME || 'turing-agents-dev'` with `PutItemCommand` and `DeleteItemCommand` in two functions and `GetItemCommand` and `QueryCommand` in two others, in a file importing `@aws-sdk/client-dynamodb`
- **THEN** the writers provide and the readers use `dynamo:turing-agents-dev`, each with `target_label` `DYNAMODB_TABLE_NAME`

#### Scenario: One variable with two defaults names two tables
- **GIVEN** `process.env.DYNAMODB_TABLE_NAME ?? 'turing-agents-dev'` and `process.env['DYNAMODB_TABLE_NAME'] || process.env.DYNAMODB_TABLE || 'wiki-agent-memory'`, each written through a dynamically imported command
- **THEN** the facts name `dynamo:turing-agents-dev` and `dynamo:wiki-agent-memory`, the second with `target_label` `DYNAMODB_TABLE_NAME,DYNAMODB_TABLE`

#### Scenario: Unreadable names and files without the SDK record nothing
- **GIVEN** `TableName: this.tableName`, a parameter, a module constant shadowed by a parameter or a local, `config.tableName || 'x'`, `process.env.X` with no default, a template `turing-agents-${env}`, `'ab'`, a `BatchWriteCommand`, a file importing `PutItemCommand` from `./fake-dynamo`, and a `*.test.ts` file
- **THEN** none of them records a fact

## MODIFIED Requirements

### Requirement: Generic contract facts mint repo-free contract nodes
`resolve_contracts` SHALL read `provides_contract` and `uses_contract` raw relations whose context is `<kind>:<name>`, kind one of `table`, `label`, `header`, `claim`, `env`, `dynamo`, and whose `target_label` is, for `table` and `label`, the database the extractor knows the name lives in (empty when it knows none) and, for `dynamo`, the comma-separated env variables whose default the name is (empty for a literal name). It SHALL mint one node per contract id, never passed through `make_id`: `table:<database>:<name>` and `label:<database>:<name>` with database `local` when none is known, `header:<name lowercased>`, `claim:<name>`, `env:<name>`, `dynamo:<name>` (case-sensitive). The node's kind SHALL be the contract kind, its label the name as a provider spells it (a user's spelling when this repo has no provider, then with `served: false` and no source anchor), with properties `name`, for tables and labels `database`, and for a DynamoDB table named by an env default `env` (every variable its facts name, in order of first appearance, comma-separated). Each provider SHALL get `handled_by` from the contract and `contains` from the provider's file; each user SHALL get `CONSUMES` to the contract. A fact with an unknown kind, no `:`, an empty name, a database spelled with `:` or spelled `local`, or a source no node names SHALL mint nothing and be counted. Extractors SHALL fold unquoted SQL identifiers to lower case and drop schema qualifiers (`public.users` is `users`) before emitting a fact. `route_resolution` SHALL report `contract_facts`, `contract_facts_unresolved`, `contracts_provided`, `contracts_external` and `contract_consumes`, through both stats JSON functions. These raw relations SHALL never become code-graph edges, and nodes of these kinds SHALL never be merged by semantic dedup.

#### Scenario: A table, a header and used-only contracts
- **GIVEN** `createUsers` providing `table:users`, `readTenant` providing `header:X-Tenant-Id`, `listUsers` using `table:users`, `sendTenant` using `header:x-tenant-id`, `claim:org_id` and `env:ML_BACKEND_URL`
- **THEN** `table:local:users` (kind `table`, database `local`) is `handled_by` `createUsers`, contained by its file and consumed by `listUsers`; `header:x-tenant-id` is labelled `X-Tenant-Id`, `handled_by` `readTenant` and consumed by `sendTenant`; `claim:org_id` and `env:ML_BACKEND_URL` are `served: false` with no anchor

#### Scenario: A DynamoDB table is its own kind
- **GIVEN** `storeTokens` and `putConnection` providing `dynamo:turing-agents-dev` with env `DYNAMODB_TABLE_NAME` and `DYNAMODB_TABLE_NAME,TABLE`, `getConnection` using it, `remember` providing `dynamo:wiki-agent-memory` with env `DYNAMODB_TABLE_NAME`, and `getConnection` using `dynamo:Sessions`
- **THEN** `dynamo:turing-agents-dev` (kind `dynamo`, no `database`, `env` `DYNAMODB_TABLE_NAME,TABLE`) is `handled_by` both writers and consumed by `getConnection`; `dynamo:wiki-agent-memory` is a separate node; `dynamo:Sessions` is `served: false` and no `dynamo:sessions` exists; no `table:` node is minted

#### Scenario: A malformed fact is counted, not minted
- **GIVEN** facts `queue:jobs`, `users`, `claim:`, a table whose database is `a:b`, a table whose database is `local`, and a header fact whose source names no node
- **THEN** no node is minted for them and `contract_facts_unresolved` is 6

#### Scenario: A contract fact is never a code-graph edge
- **GIVEN** a `uses_contract` fact whose `target_label` names a type the source file imports
- **THEN** `resolve_raw_relations` adds no edge

#### Scenario: Near-identical contract names stay apart
- **GIVEN** `table:formulation_values` and `table:formulation_value` anchored at the same migration lines, and likewise near-identical labels, headers, claims, env names and DynamoDB tables
- **THEN** semantic dedup merges none of them

### Requirement: One rule says which contract ids cross repositories
`is_bridged_contract(id)` SHALL be true for every `endpoint:` and `dynamo:` id (a DynamoDB table's name is its whole address in an AWS account and region: there is no database between to declare), for a `claim:` id whose name is not a standard JWT claim (`is_standard_jwt_claim`: a sorted static table of the IANA JSON Web Token Claims registry entries defined by RFC 7519 section 4.1, OpenID Connect Core 1.0, OpenID Connect Front-Channel Logout 1.0, RFC 7800, RFC 8693 and RFC 9449, retrieval date recorded; case-sensitive), for a `header:` id whose name is not a standard HTTP header, and for a `table:` or `label:` id in a named database; it SHALL be false for `table:local:` and `label:local:` ids, for every `env:` id, for a standard claim, for a standard header and for every non-contract id. The standard headers SHALL be a sorted static table of the permanent entries of the IANA HTTP Field Name Registry (lowercased, `*` dropped, retrieval date recorded) plus `x-request-id`, `x-real-ip`, `x-correlation-id`, `traceparent`, `tracestate`, `baggage`, and every `x-forwarded-*` name. `crossing_id` (with the declared databases and env) SHALL give the id a repo's contract crosses at: a member's repo-local table at its database's id, a declared env id as itself, a bridged id as itself, else none; `contract_spellings` SHALL give every id a repo may hold a crossing under. Seam discovery and fuse, workspace `impact` and `path`, and change context's `cross_service` SHALL use these in place of any `endpoint:` prefix test. Proxy prefixes remain endpoint-only.

#### Scenario: Repo-local tables, env names and standard headers do not cross by themselves
- **THEN** `is_bridged_contract` is false for `table:local:users`, `env:NODE_ENV`, `header:authorization`, `header:x-forwarded-for` and `header:traceparent`, and true for `table:turing:orders`, `header:x-tenant-id`, `claim:org_id` and `dynamo:turing-agents-dev`

#### Scenario: Declarations make a repo-local table and an env name cross
- **GIVEN** database `turing` of api and ml, and `API_URL` declared for api
- **THEN** `crossing_id` gives ml's `table:local:users` as `table:turing:users`, `env:API_URL` as itself, and nothing for `env:NODE_ENV`; ml's spellings of `table:turing:users` are `table:turing:users` and `table:local:users`, web's only `table:turing:users`

#### Scenario: Standard JWT claims never cross
- **THEN** `is_bridged_contract` is false for `claim:iss`, `claim:sub`, `claim:aud`, `claim:exp`, `claim:nbf`, `claim:iat`, `claim:jti`, `claim:email`, `claim:name`, `claim:preferred_username`, `claim:scope`, `claim:client_id`, `claim:azp`, `claim:nonce` and `claim:sid`, and true for `claim:roles`, `claim:session_id`, `claim:tenant_id`, `claim:permissions` and `claim:EXP`

