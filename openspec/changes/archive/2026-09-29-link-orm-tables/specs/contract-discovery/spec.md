## ADDED Requirements

### Requirement: An ORM model maps the SQL table its migration creates
When a module-level JavaScript or TypeScript variable is initialised by a Drizzle table constructor (`pgTable`, `mysqlTable` or `sqliteTable`, through type wrappers such as `as`) whose first argument is a string literal, `resolve_contracts` SHALL add a `maps_table` edge from that variable to the `sql_table` node of the same name (compared the way node ids are, so case differences do not matter). It SHALL add no edge when no `sql_table` node of that name exists, when the name is not a string literal, or when the constructor is any other call. The fact SHALL NOT be resolved as a type reference.

#### Scenario: A Drizzle model maps its migration's table
- **GIVEN** a migration with `CREATE TABLE "competitors"` and `export const competitors = pgTable('competitors', {...})`
- **THEN** the graph has a `maps_table` edge from the `competitors` variable to `sql_table:competitors`

#### Scenario: Quoted mixed-case names and the other dialects map
- **GIVEN** `CREATE TABLE IF NOT EXISTS "User_Library_Favorites"` with `pgTable("User_Library_Favorites", ...) as unknown as Table`, and `CREATE TABLE "sessions"` with `mysqlTable('sessions', ...)`
- **THEN** both variables map their tables

#### Scenario: Names that are not migrated, not literal, or not Drizzle map nothing
- **GIVEN** `pgTable('not_migrated', ...)`, `pgTable(sessions, ...)` where `sessions` is a constant, and `makeTable('competitors', ...)`
- **THEN** none of them has a `maps_table` edge
