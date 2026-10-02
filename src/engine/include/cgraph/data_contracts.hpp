#pragma once

#include "cgraph/extractor.hpp"
#include "cgraph/language_config.hpp"
#include "cgraph/types.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>

// Data contracts (CGraph Phase 3.1): the SQL tables and graph-database labels a
// repository provides or uses, as `provides_contract` / `uses_contract` facts
// (contracts.hpp). Every fact names no database: a table or label is
// `table:local:<name>` / `label:local:<name>` in its repo's graph and crosses
// into another repo only where a declaration puts both in one database.
//
// Providers (the repo owns the schema):
//   - a `sql_table` a .sql file creates (`CREATE TABLE "measurements"`), from
//     that table's node;
//   - a Drizzle model (`pgTable('projects', ...)`, mysqlTable, sqliteTable) in a
//     repo whose own .sql files create the same table, from the model variable;
//   - a Spring Data Neo4j `@Node("User")` class (no value: the class name),
//     from the class: `label:User`;
//   - an outgoing `@Relationship(type = "HAS_ROLE")` on a property of that class:
//     `label:User.HAS_ROLE`, the relationship type qualified by the label of the
//     node it starts at, so the four entities that each declare a `HAS_ROLE`
//     are four contracts and a Cypher `(u:User)-[:HAS_ROLE]->(r)` uses User's.
//     An INCOMING or UNDIRECTED relationship starts at the other entity, whose
//     label the declaring class does not know: it provides nothing.
//
// Users (the repo reads or writes another's schema):
//   - a Drizzle model in a repo whose .sql files do not create the table: a
//     mirrored schema (`pgTable("compiq_jobs")` copied into a second service);
//     the model variable and every function passing that variable to a Drizzle
//     query builder (`db.update(compiqJobs)`, `.innerJoin(compiqReports, ...)`)
//     use `table:compiq_jobs`;
//   - a table named in SQL inside a string literal (Python, JavaScript /
//     TypeScript, Kotlin, Java), adjacent literals and `+` chains read as one
//     text, interpolations as an opaque placeholder (a name ending in `_`
//     before one, or with more name after it, is unreadable: `events_{year}`,
//     `events_%s`, `t{y}_x`; `users{where_sql}`, `users$filter` and
//     `"... FROM users" + where` read `users`): the text must open, after
//     spaces, `(` and leading SQL comments, with an
//     upper-case SELECT / INSERT / UPDATE / DELETE / WITH, and the table follows
//     an upper-case FROM, JOIN, INSERT INTO, UPDATE ... SET or DELETE FROM. A
//     JPA `@Query` without `nativeQuery = true` and a `@NamedQuery` hold JPQL
//     (entities, not tables) and are not read as SQL; `JOIN u.roles` after an
//     alias `u` is a path, not a table;
//   - a label or relationship type in Cypher: a `.cypher` file (not `.cql`,
//     which is also Cassandra's), or a string literal opening with an
//     upper-case MATCH / OPTIONAL MATCH / MERGE / CREATE ( / UNWIND.
//     Commented-out Cypher is ignored.
//
// Test files (a `test`, `tests`, `__tests__`, `__mocks__`, `mocks`, `spec`,
// `specs`, `testdata`, `testutil`, `fixtures`, `testFixtures`,
// `integrationTest`, `e2e` or `cypress` directory, `*.test.*`, `*.spec.*`,
// Python `test_*.py` / `*_test.py` / `conftest.py`, Go `*_test.go`, a
// `scripts/mock-*` server, Kotlin / Java `FooTest`, `FooTests`, `FooIT`) are
// not read at all by extract_code_data_contracts, nor by the header contract
// walks (header_contracts.hpp), and code in them does not reach a header read
// (contracts.hpp): their strings are queries a test feeds a
// SQL generator or a fixture database, and their entities and query builders
// are fixtures, not the service's schema. Migrations and Drizzle models are
// read everywhere: a `.sql` file or a `pgTable` is a schema wherever it sits.
namespace cgraph {

// A SQL text's table names, normalized as contracts.hpp says (unquoted folded
// to lower case, schema qualifier dropped, quoted spelling kept), in order of
// first appearance. Empty unless the text opens with an upper-case statement
// verb. `?` stands for an interpolation the extractor could not read.
[[nodiscard]] std::vector<std::string> sql_text_tables(std::string_view text);

// The contract names a Cypher text uses (`User`, `User.HAS_ROLE`), in order of
// first appearance. Comments are skipped; a relationship type is named only
// when the label of its start node is known in the same statement.
// `require_clause` demands the text open with an upper-case Cypher clause (a
// string literal in code); a .cypher file passes false.
[[nodiscard]] std::vector<std::string> cypher_text_labels(std::string_view text, bool require_clause);

// Whether a repo-relative path is a test file (above).
[[nodiscard]] bool is_test_source_path(std::string_view relative_path);

// Facts for one parsed source file of a tree-sitter language: SQL and Cypher
// string literals for every language named above, Spring Data Neo4j
// annotations for Kotlin and Java, Drizzle query-builder table arguments
// (`orm_table_use` raw relations: source = the enclosing function, target_label
// = the identifier) for JavaScript and TypeScript. Facts hang off the innermost
// enclosing function (else module-level variable, else class, else file) node
// already in `fragment`.
void extract_code_data_contracts(const TSNode& root, std::string_view language, const ExtractionContext& context,
                                 const Fragment& fragment, std::vector<RawRelation>& raw_relations);

// `provides_contract` facts for every `sql_table` node an extract_sql fragment holds.
[[nodiscard]] std::vector<RawRelation> sql_table_contract_facts(const Fragment& fragment,
                                                                const ExtractionContext& context);

// A `.cypher` file: its file node and the labels it uses.
[[nodiscard]] ExtractionResult extract_cypher(const ExtractionContext& context);

// Facts only the whole graph can decide, for resolve_contracts: a Drizzle model
// (`maps_table`) provides its table when `has_sql_table(name)` (the repo's own
// .sql files create it) and uses it otherwise; an `orm_table_use` whose
// identifier `resolve_name` resolves to a model of a table the repo does not
// create uses that table.
[[nodiscard]] std::vector<RawRelation> orm_table_contract_facts(
    std::span<const RawRelation> raw_relations, const std::function<bool(const std::string&)>& has_sql_table,
    const std::function<std::string(const RawRelation&)>& resolve_name);

}  // namespace cgraph
