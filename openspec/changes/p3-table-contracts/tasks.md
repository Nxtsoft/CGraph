## 1. Extraction

- [x] 1.1 `data_contracts.cpp`: SQL text tables, Cypher text labels, test-path rule.
- [x] 1.2 String literals and concatenations in Python, JavaScript / TypeScript, Kotlin, Java; facts hang off the innermost function, else module variable, else class, else file.
- [x] 1.3 Spring Data Neo4j `@Node` / outgoing `@Relationship` providers.
- [x] 1.4 `.cypher` detected as `Cypher` (`.cql` not: Cassandra), extracted to a file node and its labels.
- [x] 1.5 `extract_sql` provides every table it creates.

## 2. Resolution

- [x] 2.1 `orm_table_contract_facts`: a Drizzle model provides its table when a `.sql` file creates it, uses it otherwise; `orm_table_use` query-builder arguments resolved through imports use a mirrored table.
- [x] 2.2 `resolve_contracts` step 8 reads the derived facts; `graph_builder` skips `orm_table_use`.
- [x] 2.3 Index version `logic-15`.

## 3. Verification

- [x] 3.1 `data_contracts_test.cpp` (fails on main: header missing; a copy without the new-API tests fails at runtime) and `index_persistence_test.cpp`.
- [x] 3.2 Probe graphs: no node or edge lost or changed; every new cross-repo join hand-checked.
- [x] 3.3 Seam + scorer with `--database turing=turing-api,ml-backend,turing-agents` and `--database idp=idp,idp-front-end`.
