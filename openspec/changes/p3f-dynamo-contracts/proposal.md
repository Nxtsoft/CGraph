# DynamoDB tables as their own contract kind (Phase 3, T34/T35)

## Why

The tables track (#162) left DynamoDB out: putting a DynamoDB table under `table:` could join a Postgres table of the same name, and SQL tables cross only inside a declared database. The probe ground truth has two DynamoDB edges, both on one table that three services share: T34 (turing-webapp reads connector records, turing-api writes Google tokens) and T35 (turing-agents `GetItem`, turing-api `PutItem`). All three name it `process.env.DYNAMODB_TABLE_NAME || 'turing-agents-dev'`. A fourth reader of the same variable in turing-agents defaults to `'wiki-agent-memory'` and is a different table.

## What Changes

- New contract kind `dynamo` in the foundation (`contracts.hpp/.cpp`): id `dynamo:<name>`, case-sensitive, no database scope. `is_bridged_contract` is true for it. Semantic dedup already exempts every contract kind. No new raw relation: the facts are `provides_contract` / `uses_contract`, which graph_builder already skips.
- `resolve_contracts` keeps a DynamoDB fact's `target_label` (the env variables the name is the default of) as the node's `env` property.
- New `dynamo_contracts.cpp` (+ header), hooked into `js_extra_walk` with one line. JavaScript/TypeScript calls with a `TableName` in a file that imports the DynamoDB SDK: writes (`PutItemCommand`, `UpdateItemCommand`, `DeleteItemCommand`, `CreateTableCommand`, lib-dynamodb `PutCommand` / `UpdateCommand` / `DeleteCommand`, DocumentClient `put` / `update` / `delete`, v2 `putItem` / `updateItem` / `deleteItem` / `createTable`) provide; reads (`GetItemCommand`, `QueryCommand`, `ScanCommand`, `GetCommand`, `get` / `query` / `scan` / `getItem`) use.
- Table name: a literal, the literal default of `process.env.X || ... || 'name'` (or `??`), or a module-level `const` holding one of those (not through a shadowing parameter or local).
- Seam discover's summary line names DynamoDB tables; README documents the kind.
- Index version `logic-19`.

## Design choices

- **Bridged by name, no declaration.** A DynamoDB table name is the table's whole address inside an AWS account and region (its ARN ends `table/<name>`); there is no database level whose sharing code fails to prove, which is what made SQL tables need `databases`. The kind keeps DynamoDB names away from SQL table names.
- **Writer provides, reader uses.** DynamoDB has no migration that owns a table's shape, and the pinned repos define none of the shared table in code (the infra lives elsewhere; idp's CDK stack names its table from a template). What a reader depends on is what a writer puts there. This matches both labels: T34's provider turing-api writes (`PutItemCommand`), its consumer turing-webapp reads; T35's provider turing-api `PutItem`, its consumer turing-agents `GetItem`. A service that both writes and reads (turing-webapp, turing-agents `saveGmailTokens`) provides and uses, which also joins it with the other writers; that is true of a shared store.
- **Env default is the name; env names are kept, never joined on.** `process.env.DYNAMODB_TABLE_NAME || 'turing-agents-dev'` is `dynamo:turing-agents-dev` with `env: DYNAMODB_TABLE_NAME`. Keying on the name means two reads of one variable with different defaults (`turing-agents-dev`, `wiki-agent-memory`) never join.
- **Only provable names.** `this.tableName` (turing-agents `DynamoDBStore`), parameters, a non-env fallback (`config?.tableName || ...`), an env read with no default and an interpolated name record nothing. A file must import the SDK, so a `put({ TableName })` on anything else is not read.
- **Test sources record nothing**, as for the other contract kinds.

## Non-goals

- Python boto3 (`resource.Table(name)`, `client.put_item(TableName=...)`): ml-backend's tables are passed by parameter and are repo-local; no labelled edge needs it.
- Infrastructure definitions (CDK `new dynamodb.Table`, CloudFormation, Terraform) as providers.
- Batch and transaction requests (`BatchWriteItem`, `TransactWriteItems`), keyed by table names inside the request.
- Item keys (`PK` `PREF#...`, `SK` `GOOGLE_TOKENS` vs `GMAIL_TOKENS`): the T35 sort-key mismatch is item-level and out of scope.
- Imported table-name constants.
