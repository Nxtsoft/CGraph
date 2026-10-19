#pragma once

// DynamoDB tables as contracts (contracts.hpp: `dynamo:<name>`). A DynamoDB
// table is a store several services share: one writes items another reads.
// It is its own kind, never a `table:`, so a DynamoDB table and a Postgres
// table of the same name stay apart.
//
// A table name is the whole address of a DynamoDB table inside an AWS account
// and region (its ARN is `...:table/<name>`); there is no database between the
// account and the table to declare. So a `dynamo:` id bridges repositories by
// name, as a non-standard header does, with no declaration.
//
// Caveat: nothing in code says which AWS account or region a service talks to,
// so a `dynamo:` name joins every repository given to a workspace or seam that
// names it, whatever account each runs in: two services in different accounts
// each with a `sessions` table would join. An optional account declaration
// (like `databases` for SQL tables) is a recorded follow-up.
//
// Who provides: DynamoDB has no migration that owns a table's shape, and the
// probe repos define none of the shared table in code; the items in it are
// what a writer puts there. A call that WRITES the table provides it
// (`provides_contract`), a call that only READS it uses it (`uses_contract`).
// A service that both writes and reads provides and uses it.
//
//   provides  v3 `PutItemCommand`, `UpdateItemCommand`, `DeleteItemCommand`,
//             `CreateTableCommand`, lib-dynamodb `PutCommand`, `UpdateCommand`,
//             `DeleteCommand`; DocumentClient / DynamoDBDocument `put`,
//             `update`, `delete`, v2 `putItem`, `updateItem`, `deleteItem`,
//             `createTable`;
//   uses      v3 `GetItemCommand`, `QueryCommand`, `ScanCommand`, lib-dynamodb
//             `GetCommand`; DocumentClient `get`, `query`, `scan`, v2
//             `getItem`; paginators `paginateQuery` / `paginateScan`, whose
//             second argument holds `TableName`.
//
// A command may be named off a namespace import (`new ddb.PutItemCommand(...)`).
//
// The call's first argument must be an object literal with a `TableName`
// property, and the file must import the DynamoDB SDK (`@aws-sdk/client-dynamodb`,
// `@aws-sdk/lib-dynamodb`, `aws-sdk/clients/dynamodb` or `aws-sdk`, by
// `import`, `require` or a dynamic `import()`). Batch and transaction calls
// (several tables keyed in one request) are not read.
//
// The table name is read from `TableName` as:
//   - a string literal (a template with no substitution);
//   - `process.env.X || 'name'` / `?? 'name'` (any chain of env reads ending in
//     a literal): the literal default is the name and the env variables are
//     recorded as the fact's target_label (comma-separated, in order), which
//     resolve_contracts keeps as the node's `env` property. Two services reading
//     one variable with different defaults (`turing-agents-dev` and
//     `wiki-agent-memory`) name two tables and never join;
//   - an identifier naming a module-level `const` of the same file whose value
//     is one of the above, unless a parameter or local of an enclosing scope
//     binds the same name, plainly or destructured (`({ TABLE }) => ...`).
// Anything else (`this.tableName`, a parameter, an imported constant, an env
// read with no default) records nothing. A name must be a valid DynamoDB table
// name (`[A-Za-z0-9_.-]`, 3 to 255 characters); it is case-sensitive.
//
// The fact's source is the innermost enclosing function, else
// js_syntax::reading_scope_id (a module-level variable with a node, else the
// file). A test source (data_contracts.hpp is_test_source_path) records
// nothing. Item keys (`PK`, `SK`) are not read.

#include "cgraph/language_config.hpp"

#include <tree_sitter/api.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// Held while one JavaScript/TypeScript file is extracted: the file's SDK imports
// and module constants are read once, on its first DynamoDB call, instead of
// per call, and the file's dynamo facts are kept as a set (relation_keys.hpp)
// instead of rescanned for each new call. Scopes nest; each covers one file on
// its thread. Without one each call reads the file itself and scans the file's
// relations.
class DynamoContractsFileScope {
 public:
  DynamoContractsFileScope();
  ~DynamoContractsFileScope();
  DynamoContractsFileScope(const DynamoContractsFileScope&) = delete;
  DynamoContractsFileScope& operator=(const DynamoContractsFileScope&) = delete;

 private:
  struct Index;
  std::unique_ptr<Index> index_;
};

// Test hook: how many relations the one-fact-per-symbol-and-table check has
// read on this thread, so a test can prove a held scope reads each once.
struct DynamoLookupCounts {
  std::size_t fact_reads = 0;
};
[[nodiscard]] DynamoLookupCounts dynamo_lookup_counts();

// True for a valid DynamoDB table name: 3 to 255 of `[A-Za-z0-9_.-]`.
[[nodiscard]] bool is_dynamo_table_name(std::string_view name);

// JavaScript / TypeScript: called from js_extra_walk on every node.
void js_dynamo_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         const Fragment& fragment, std::vector<RawRelation>& out);

}  // namespace cgraph
