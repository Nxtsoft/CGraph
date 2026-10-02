## ADDED Requirements

### Requirement: A repository's schema provides table and graph-label contracts
Extraction SHALL emit `provides_contract` facts naming no database for: every table a `.sql` file creates (`table:<name>`, from its `sql_table` node); a Spring Data Neo4j `@Node` class in Kotlin or Java (`label:<value>` for each string value, the class's simple name when there is none); and an outgoing `@Relationship` (no `direction`, or `OUTGOING`) on a member of that class, `label:<first label>.<type>`. An `INCOMING` or `UNDIRECTED` relationship SHALL provide nothing. `resolve_contracts` SHALL make a Drizzle model (`maps_table`) a provider of `table:<name>` when an `sql_table` node of that name exists in the graph.

#### Scenario: A migration and its Drizzle model both provide the table
- **GIVEN** `CREATE TABLE projects` in a `.sql` file and `export const projects = pgTable('projects', ...)`
- **THEN** `table:local:projects` is `handled_by` both the `sql_table` node and the `projects` variable

#### Scenario: Neo4j entities provide labels and start-qualified relationship types
- **GIVEN** `@Node("User")` with `@Relationship(type = "HAS_ROLE")` and an `INCOMING` `HAS_SESSION`, `@Node("Client")` with `@Relationship(type = "HAS_ROLE")`, and a Java `@Node` class `Role` with `@Relationship("COMPOSED_OF")`
- **THEN** the contracts are `label:local:User`, `User.HAS_ROLE`, `Client`, `Client.HAS_ROLE`, `Role` and `Role.COMPOSED_OF`, and nothing for `HAS_SESSION`

### Requirement: Code that reads another schema uses its tables and labels
Extraction SHALL emit `uses_contract` facts naming no database, hung off the innermost enclosing function (else module-level variable, else class, else file) node:
- for each table after an upper-case `FROM`, `JOIN`, `INSERT INTO` or `UPDATE <table> [alias] SET` in a Python, JavaScript / TypeScript, Kotlin or Java string literal, adjacent-literal concatenation or `+` chain whose text opens with an upper-case `SELECT`, `INSERT`, `UPDATE`, `DELETE` or `WITH`; interpolations and non-literal operands SHALL be opaque, unquoted names folded to lower case, quoted names kept, schema qualifiers dropped; CTE names, subqueries, set-returning functions, names inside function-call parentheses, `IS DISTINCT FROM` operands, bind parameters and `information_schema` / `pg_catalog` / `pg_*` names SHALL name no table;
- for each node label and each start-qualified relationship type (`<start label>.<TYPE>`) in Cypher: a `.cypher` / `.cql` file, or a string literal opening with an upper-case `MATCH`, `OPTIONAL MATCH`, `MERGE`, `CREATE (` or `UNWIND`; comments and string contents SHALL be ignored, a relationship SHALL be named only when its start node's label is known inline or from a variable bound in the same `;`-separated statement, and an undirected pattern SHALL name no relationship;
- in JavaScript / TypeScript, `orm_table_use` from a function passing an identifier to a Drizzle query builder (`from`, `update`, `insert`, `delete`, `join`, `innerJoin`, `leftJoin`, `rightJoin`, `fullJoin`).

String literals in test files (a `test`, `tests`, `__tests__`, `spec`, `specs`, `testdata` or `fixtures` directory, `*.test.*`, `*.spec.*`, Python `test_*.py` / `*_test.py` / `conftest.py`, Kotlin / Java `*Test`, `*Tests`, `*IT`) SHALL NOT be read. `resolve_contracts` SHALL make a Drizzle model whose table no `sql_table` node holds a user of `table:<name>`, and every `orm_table_use` whose identifier resolves (through the file's imports, then its own variables) to such a model a user of it.

#### Scenario: SQL built across adjacent Python literals
- **GIVEN** a module-level helper returning `(f"SELECT {cols} " "FROM measurements m " f"{clause}" "WHERE ...")` and a method calling `text("SELECT p.id " "FROM projects p " ...)`
- **THEN** the helper uses `table:local:measurements` and the method uses `table:local:projects`, while `"Pick a project from the list"` and a test file's `"SELECT id FROM users"` use nothing

#### Scenario: A mirrored Drizzle schema is a consumer
- **GIVEN** a repo with no `.sql` files declaring `pgTable("compiq_jobs")` and `pgTable("compiq_reports")`, and functions calling `db.update(compiqJobs)` and `.from(compiqJobs).innerJoin(compiqReports, ...)` through an import
- **THEN** `table:local:compiq_jobs` has no provider and is used by the model and both functions; `table:local:compiq_reports` by its model and the joining function; `Array.from(items)` uses nothing

#### Scenario: Cypher uses a relationship of its start label only
- **GIVEN** a `.cypher` script with a commented-out `MERGE (u:Ghost ...)` and a live `MATCH (u:User ...) MATCH (r:Role ...) MERGE (u)-[:HAS_ROLE]->(r)`, and a `@Query("MATCH (u:User)-[:HAS_ROLE]->(r:Role) ...")`
- **THEN** both use `label:local:User`, `label:local:Role` and `label:local:User.HAS_ROLE`, nothing uses `Ghost` or `Client.HAS_ROLE`
