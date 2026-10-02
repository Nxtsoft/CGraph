#include "cgraph/data_contracts.hpp"

#include "cgraph/configured_extractors.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/non_grammar_extractors.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << "data_contracts_test: " << message << '\n';
  return 1;
}

std::string joined(const std::vector<std::string>& values) {
  std::string out;
  for (const auto& value : values) {
    out += (out.empty() ? "" : ",") + value;
  }
  return out;
}

// Extracts with the real extractors, merges, resolves imports and contracts:
// the order run_one_shot uses.
cgraph::GraphSnapshot build(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<cgraph::Fragment> fragments;
  std::vector<cgraph::RawRelation> relations;
  for (const auto& [path, source] : files) {
    const cgraph::ExtractionContext context{.source_file = path, .relative_path = path, .source = source};
    const auto language = cgraph::detect_language(path);
    const auto result = language == cgraph::DetectedLanguage::Sql || language == cgraph::DetectedLanguage::Cypher
                            ? *cgraph::extract_non_grammar_language(language, context)
                        : language == cgraph::DetectedLanguage::Kotlin || language == cgraph::DetectedLanguage::Java
                            ? *cgraph::extract_configured_language(language, context)
                        : language == cgraph::DetectedLanguage::Python ? cgraph::extract_python(context)
                                                                       : cgraph::extract_typescript(context);
    fragments.push_back(result.fragment);
    relations.insert(relations.end(), result.raw_relations.begin(), result.raw_relations.end());
  }
  auto graph = cgraph::merge_fragments(fragments);
  cgraph::resolve_imports(graph);
  cgraph::resolve_contracts(graph, relations);
  return graph;
}

bool has_edge(const cgraph::GraphSnapshot& graph, std::string_view source, std::string_view target,
              std::string_view relation) {
  return std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
    return edge.source == source && edge.target == target && edge.relation == relation;
  });
}

// The code nodes (by label) holding `relation` edges with a contract.
std::vector<std::string> sides(const cgraph::GraphSnapshot& graph, std::string_view contract, std::string_view relation) {
  std::vector<std::string> labels;
  for (const auto& edge : graph.edges) {
    const bool provider = relation == "handled_by" && edge.source == contract && edge.relation == relation;
    const bool user = relation == "CONSUMES" && edge.target == contract && edge.relation == relation;
    if (!provider && !user) {
      continue;
    }
    const auto& id = provider ? edge.target : edge.source;
    for (const auto& node : graph.nodes) {
      if (node.id == id) {
        labels.push_back(node.label);
      }
    }
  }
  std::ranges::sort(labels);
  return labels;
}

std::vector<std::string> contract_ids(const cgraph::GraphSnapshot& graph) {
  std::vector<std::string> ids;
  for (const auto& node : graph.nodes) {
    if (node.kind == "table" || node.kind == "label") {
      ids.push_back(node.id);
    }
  }
  std::ranges::sort(ids);
  return ids;
}

int test_sql_text_tables() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      // ml-backend's shape: the projection interpolated, joins on following lines.
      {"SELECT  ? FROM measurements m JOIN measurement_outcomes mo ON mo.measurement_id = m.id "
       "LEFT JOIN users u ON u.id = m.created_by_id",
       "measurements,measurement_outcomes,users"},
      {"INSERT INTO audit_log (id, at) VALUES (:id, now()) ON CONFLICT (id) DO UPDATE SET at = now()", "audit_log"},
      {"UPDATE compiq_jobs SET status = 'done' WHERE id = $1", "compiq_jobs"},
      {"UPDATE compiq_jobs AS j SET status = 'done'", "compiq_jobs"},
      {"DELETE FROM sessions WHERE expires_at < now()", "sessions"},
      {"SELECT * FROM jobs WHERE id = $1 FOR UPDATE SKIP LOCKED", "jobs"},
      // Unquoted names fold, the schema drops, a quoted name keeps its case.
      {"SELECT 1 FROM public.Projects p JOIN \"Users\" u ON true", "projects,Users"},
      // A CTE, a subquery, a set-returning function and FROM inside a function call are no tables.
      {"WITH recent AS (SELECT id FROM orders WHERE EXTRACT(YEAR FROM created_at) = 2026) "
       "SELECT * FROM recent JOIN (SELECT * FROM items) i ON true CROSS JOIN unnest(:ids) x",
       "orders,items"},
      {"SELECT a IS DISTINCT FROM b FROM pairs", "pairs"},
      {"SELECT table_name FROM information_schema.tables JOIN pg_class c ON true", ""},
      // A placeholder or bind parameter where the table stands names nothing.
      {"SELECT * FROM  ?  WHERE id = 1", ""},
      {"SELECT * FROM :table", ""},
      // Not SQL: prose, a lower-case or mid-sentence verb, a lone fragment.
      {"Only SELECT queries are allowed. Received from the user", ""},
      {"SELECT INTO is not allowed. Only read-only SELECT queries are permitted.", ""},
      {"select id from users", ""},
      // JPQL: `JOIN u.roles` after the alias `u` is a path through the entity.
      {"SELECT u FROM User u JOIN u.roles r JOIN public.grants g ON true", "user,grants"},
      // A name glued to an interpolation (\x01) or a format directive is unreadable.
      {"SELECT * FROM measurements_\x01 m JOIN outcomes o ON true", "outcomes"},
      {"SELECT * FROM events_%s", ""},
      {"SELECT * FROM \x01_archive", ""},
      {"SELECT * FROM {prefix}_events", ""},
      // A name complete before an interpolation or a `+` boundary (\x02) is read;
      // one ending in `_`, or continued after it, is not.
      {"SELECT * FROM users\x01", "users"},
      {"SELECT * FROM users\x01 WHERE id = 1", "users"},
      {"SELECT * FROM users{where_sql}", "users"},
      {"SELECT * FROM users%s", "users"},
      {"SELECT * FROM t\x01_2024", ""},
      {"SELECT * FROM users\x02", "users"},
      {"SELECT * FROM events_\x02", ""},
      {"SELECT * FROM \x02_archive JOIN \x02 x ON true JOIN items i ON true", "items"},
      // Leading SQL comments before the verb.
      {"-- name: list users\nSELECT * FROM users", "users"},
      {"/* annotated */ SELECT * FROM users", "users"},
      // CTEs with a column list or a materialization hint are no tables.
      {"WITH x AS MATERIALIZED (SELECT 1 FROM a) SELECT * FROM x", "a"},
      {"WITH x AS NOT MATERIALIZED (SELECT 1 FROM a), y(c, d) AS (SELECT 1, 2 FROM b) SELECT * FROM x JOIN y ON true",
       "a,b"},
      {"FROM projects p ", ""},
      {"Select a project from the list", ""},
  };
  for (const auto& [text, want] : cases) {
    if (const auto got = joined(cgraph::sql_text_tables(text)); got != want) {
      return fail("sql_text_tables(\"" + text + "\") = [" + got + "], want [" + want + "]");
    }
  }
  return 0;
}

int test_cypher_text_labels() {
  // A commented-out block (idp-front-end's setup script) names nothing; the
  // live statement binds u and r, so HAS_ROLE starts at User.
  const std::string script = R"(// Step 6: (only if missing)
/*
MERGE (u:Ghost {
    id: apoc.create.uuid()
})
*/
// MATCH (x:Commented)-[:NOPE]->(y)
MATCH (u:User {username: 'testadmin', tenantId: $masterTenantId})
MATCH (r:Role {roleCode: 'admin (:NotALabel)'})
MERGE (u)-[:HAS_ROLE]->(r)
RETURN 'ADMIN role assigned' AS status;

MATCH (t:Tenant)<-[:BELONGS_TO]-(c)
MATCH (a)-[:KNOWS]-(b)
MATCH (p)-[:OWNS]->(q)
RETURN size((t)-[:HAS_USER|:HAS_GROUP*1..2]->());
)";
  if (const auto got = joined(cgraph::cypher_text_labels(script, false));
      got != "User,Role,User.HAS_ROLE,Tenant,Tenant.HAS_USER,Tenant.HAS_GROUP") {
    return fail("cypher_text_labels(script) = [" + got + "]");
  }
  // `<-[:BELONGS_TO]-(c)` starts at c, whose label is unknown: no contract.
  // A variable's label does not leak across `;`.
  if (const auto got = joined(cgraph::cypher_text_labels("MATCH (u:User) RETURN u; MATCH (u)-[:HAS_ROLE]->(r)", true));
      got != "User") {
    return fail("a label bound in one statement qualified the next: [" + got + "]");
  }
  if (const auto got = joined(cgraph::cypher_text_labels("MATCH (c:Client)<-[:ISSUED_TO]-(a:AuthorizationCode)", true));
      got != "Client,AuthorizationCode.ISSUED_TO,AuthorizationCode") {
    return fail("an incoming arrow did not start at the right-hand node: [" + got + "]");
  }
  // In code a Cypher string must open with a clause; DDL and prose are not Cypher.
  for (const auto* text : {"CREATE TABLE users (id int)", "Match (u:User) here", "note (u:User)",
                           "matches (u:User)"}) {
    if (const auto got = cgraph::cypher_text_labels(text, true); !got.empty()) {
      return fail(std::string("not Cypher, but read as such: ") + text);
    }
  }
  return 0;
}

int test_test_paths() {
  for (const auto* path : {"tests/text_to_sql_agent/nodes.test.ts", "src/__tests__/x.ts", "ops/test_repo.py",
                           "ops/repo_test.py", "idp-core/src/test/kotlin/A.kt", "src/RepoIT.kt", "conftest.py",
                           "lib/hooks/use.spec.ts"}) {
    if (!cgraph::is_test_source_path(path)) {
      return fail(std::string("not seen as a test path: ") + path);
    }
  }
  for (const auto* path : {"src/testFixtures/kotlin/Seed.kt", "src/integrationTest/kotlin/Seed.kt", "e2e/login.ts",
                           "src/__mocks__/db.ts", "cypress/support/db.ts", "src/UserServiceTests.kt", "src/V2Test.java"}) {
    if (!cgraph::is_test_source_path(path)) {
      return fail(std::string("not seen as a test path: ") + path);
    }
  }
  for (const auto* path : {"ops/services/projects/repository.py", "src/AUDIT.py", "src/compiq_agent/utils/db.ts",
                           "idp-core/src/main/kotlin/User.kt", "src/latest.ts", "src/AUDIT.kt", "src/ABTest.kt",
                           "src/CONTEXT.java"}) {
    if (cgraph::is_test_source_path(path)) {
      return fail(std::string("seen as a test path: ") + path);
    }
  }
  return 0;
}

// The owner (.sql migration + Drizzle model) provides a table; a Python SQL
// string in another file uses it; a test file's string does not.
int test_tables_in_one_repo() {
  const auto graph = build({
      {"src/db/migrations/0006_ml.sql", "CREATE TABLE \"measurements\" (\n  id uuid\n);\nCREATE TABLE projects (id uuid);\n"},
      {"src/db/schema/projects.ts", "import { pgTable, uuid } from 'drizzle-orm/pg-core';\n"
                                    "export const projects = pgTable('projects', { id: uuid('id') });\n"},
      {"ops/repository.py",
       "from sqlalchemy import text\n"
       "\n"
       "def _measurement_query(select_cols, measurement_type=None):\n"
       "    clause = f\" AND m.type = '{measurement_type}' \" if measurement_type else \"\"\n"
       "    return (\n"
       "        f\"SELECT {select_cols} \"\n"
       "        \"FROM measurements m \"\n"
       "        f\"{clause}\"\n"
       "        \"WHERE m.deleted_at IS NULL\"\n"
       "    )\n"
       "\n"
       "class Repo:\n"
       "    async def get_project(self, session, project_id):\n"
       "        return await session.execute(text(\n"
       "            \"SELECT p.id, p.name \"\n"
       "            \"FROM projects p \"\n"
       "            \"WHERE p.id = :project_id\"\n"
       "        ))\n"
       "\n"
       "    def describe(self):\n"
       "        return \"Pick a project from the list\"\n"},
      {"tests/test_generator.py", "def test_sql():\n    assert run(\"SELECT id FROM users\")\n"},
  });
  const auto ids = joined(contract_ids(graph));
  if (ids != "table:local:measurements,table:local:projects") {
    return fail("contract ids: [" + ids + "]");
  }
  if (const auto got = joined(sides(graph, "table:local:projects", "handled_by")); got != "projects,projects") {
    return fail("projects providers (migration table + model): [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "table:local:projects", "CONSUMES")); got != "get_project") {
    return fail("projects users: [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "table:local:measurements", "CONSUMES")); got != "_measurement_query") {
    return fail("a SQL string built in a module-level helper did not use its table: [" + got + "]");
  }
  return 0;
}

// A Drizzle model of a table no .sql file in the repo creates mirrors another
// service's schema: the model and the functions handing it to a query builder
// use the table.
int test_mirrored_drizzle_schema() {
  const auto graph = build({
      {"src/db/schema/compiq.ts", "import { pgTable, uuid } from 'drizzle-orm/pg-core';\n"
                                  "export const compiqReports = pgTable(\"compiq_reports\", { id: uuid('id') });\n"
                                  "export const compiqJobs = pgTable(\"compiq_jobs\", { id: uuid('id') });\n"},
      {"src/utils/db.ts", "import { compiqJobs, compiqReports } from '../db/schema/compiq';\n"
                          "export async function updateJobStatus(db, jobId, data) {\n"
                          "  await db.update(compiqJobs).set(data);\n"
                          "}\n"
                          "export async function getJobContext(db, jobId) {\n"
                          "  return db.select().from(compiqJobs).innerJoin(compiqReports, eq(compiqJobs.reportId, compiqReports.id));\n"
                          "}\n"
                          "export function unrelated(items) {\n"
                          "  return Array.from(items);\n"
                          "}\n"},
  });
  const auto ids = joined(contract_ids(graph));
  if (ids != "table:local:compiq_jobs,table:local:compiq_reports") {
    return fail("mirrored contract ids: [" + ids + "]");
  }
  if (const auto got = joined(sides(graph, "table:local:compiq_jobs", "handled_by")); !got.empty()) {
    return fail("a mirrored model provided its table: [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "table:local:compiq_jobs", "CONSUMES"));
      got != "compiqJobs,getJobContext,updateJobStatus") {
    return fail("compiq_jobs users: [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "table:local:compiq_reports", "CONSUMES"));
      got != "compiqReports,getJobContext") {
    return fail("compiq_reports users: [" + got + "]");
  }
  return 0;
}

// Spring Data Neo4j entities provide labels and outgoing relationship types,
// qualified by the entity's label; a .cypher script and a @Query string use them.
int test_graph_labels() {
  const auto graph = build({
      {"entity/User.kt", "package x\n"
                         "@Node(\"User\")\n"
                         "data class User(\n"
                         "    @Id val id: String,\n"
                         ") {\n"
                         "    @Relationship(type = \"HAS_ROLE\")\n"
                         "    var roles: MutableSet<Role> = mutableSetOf()\n"
                         "    @Relationship(type = \"HAS_SESSION\", direction = Relationship.Direction.INCOMING)\n"
                         "    var sessions: MutableSet<Session> = mutableSetOf()\n"
                         "}\n"},
      {"entity/Client.kt", "package x\n"
                           "@Node(\"Client\")\n"
                           "data class Client(val id: String) {\n"
                           "    @Relationship(type = \"HAS_ROLE\")\n"
                           "    var clientRoles: MutableSet<Role> = mutableSetOf()\n"
                           "}\n"},
      {"entity/Group.kt", "package x\n"
                          "@Node(labels = [\"Group\", \"Principal\"])\n"
                          "data class Group(val id: String) {\n"
                          "    @Relationship(type = \"PEER_OF\", direction = Relationship.Direction.UNDIRECTED)\n"
                          "    var peers: MutableSet<Group> = mutableSetOf()\n"
                          "    @Relationship(type = \"HAS_ROLE\")\n"
                          "    var roles: MutableSet<Role> = mutableSetOf()\n"
                          "}\n"},
      {"entity/Role.java", "package x;\n"
                           "@Node\n"
                           "public class Role {\n"
                           "  @Relationship(\"COMPOSED_OF\") private Set<Role> composites;\n"
                           "}\n"},
      {"repository/UserRepository.kt", "package x\n"
                                       "interface UserRepository {\n"
                                       "    @Query(\"MATCH (u:User)-[:HAS_ROLE]->(r:Role) WHERE u.id = \\$id RETURN r\")\n"
                                       "    fun roles(id: String): List<Role>\n"
                                       "}\n"},
      {"scripts/cypher/setup.cypher", "/*\nMERGE (u:User {id: 1})\n*/\n"
                                      "MATCH (u:User {username: 'a'})\nMATCH (r:Role {roleCode: 'admin'})\n"
                                      "MERGE (u)-[:HAS_ROLE]->(r);\n"},
  });
  const auto ids = joined(contract_ids(graph));
  if (ids != "label:local:Client,label:local:Client.HAS_ROLE,label:local:Group,label:local:Group.HAS_ROLE,"
             "label:local:Principal,label:local:Role,label:local:Role.COMPOSED_OF,label:local:User,"
             "label:local:User.HAS_ROLE") {
    return fail("label contract ids: [" + ids + "]");
  }
  if (const auto got = joined(sides(graph, "label:local:User.HAS_ROLE", "handled_by")); got != "User") {
    return fail("User.HAS_ROLE providers: [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "label:local:User.HAS_ROLE", "CONSUMES")); got != "cypher/setup.cypher,roles") {
    return fail("User.HAS_ROLE users: [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "label:local:Client.HAS_ROLE", "CONSUMES")); !got.empty()) {
    return fail("a (u:User)-[:HAS_ROLE] pattern used Client's HAS_ROLE: [" + got + "]");
  }
  if (const auto got = joined(sides(graph, "label:local:Role", "handled_by")); got != "Role") {
    return fail("@Node with no value did not provide the class name: [" + got + "]");
  }
  if (!has_edge(graph, "label:local:User", "entity_user_kt_user", "handled_by")) {
    return fail("the User label is not handled by the User class node");
  }
  return 0;
}

// SQL strings in every language the reader covers, through the real
// extractors: `+` chains (the verb in one literal, the table in the next),
// interpolations as opaque placeholders, JPQL annotations skipped.
int test_sql_strings_per_language() {
  const auto graph = build({
      {"src/q.js", "export function listOrders(db, id) {\n"
                   "  return db.query(\"SELECT * \" + \"FROM orders o \" + \"WHERE o.id = \" + id);\n"
                   "}\n"
                   "export function byYear(db, year) {\n"
                   "  return db.query(`SELECT * FROM measurements_${year}`);\n"
                   "}\n"
                   "export function fromTemplate(db, t) {\n"
                   "  return db.query(`SELECT * FROM ${t} JOIN items i ON true`);\n"
                   "}\n"},
      {"src/q.ts", "export async function listAccounts(db: Db, y: string) {\n"
                   "  return db.execute(`SELECT id FROM accounts WHERE x = ${y}`);\n"
                   "}\n"
                   "export function filtered(db: Db, whereClause: string) {\n"
                   "  return db.query(\"SELECT * FROM members\" + whereClause);\n"
                   "}\n"},
      {"src/Repo.kt", "package x\n"
                      "class Repo(private val jdbc: Jdbc) {\n"
                      "    fun load(id: String) = jdbc.query(\"SELECT * \" + \"FROM invoices i WHERE i.id = $id\")\n"
                      "    fun tpl(t: String) = jdbc.query(\"SELECT * FROM ${t}_archive JOIN ledgers l ON true\")\n"
                      "    fun filtered(filter: String) = jdbc.query(\"SELECT * FROM credits$filter\")\n"
                      "}\n"
                      "interface UserJpa {\n"
                      "    @Query(\"SELECT u FROM User u JOIN u.roles r\")\n"
                      "    fun jpql(): List<User>\n"
                      "    @Query(value = \"SELECT * FROM app_users\", nativeQuery = true)\n"
                      "    fun native(): List<User>\n"
                      "}\n"},
      {"src/Dao.java", "package x;\n"
                       "class Dao {\n"
                       "  List<X> all(String id) { return jdbc.query(\"SELECT * \" + \"FROM payments p WHERE p.id = \" + id); }\n"
                       "  List<X> refunds(String where) { return jdbc.query(\"SELECT * FROM refunds\" + where); }\n"
                       "  @Query(\"SELECT o FROM Order o JOIN o.lines l\")\n"
                       "  List<Order> jpql() { return null; }\n"
                       "}\n"},
      {"ops/q.py", "def plus(id):\n"
                   "    return \"SELECT * \" + \"FROM shipments s WHERE s.id = \" + id\n"
                   "\n"
                   "def glued(year):\n"
                   "    return f\"SELECT * FROM measurements_{year}\"\n"
                   "\n"
                   "def pct():\n"
                   "    return \"SELECT * FROM events_%s\" % \"x\"\n"
                   "\n"
                   "def where(where_sql):\n"
                   "    return f\"SELECT * FROM parcels{where_sql}\"\n"
                   "\n"
                   "def where_plus(where_sql):\n"
                   "    return \"SELECT * FROM carriers\" + where_sql\n"},
      {"src/__mocks__/db.ts", "export function mock(db) { return db.query(\"SELECT * FROM mocked\"); }\n"},
  });
  const auto ids = joined(contract_ids(graph));
  if (ids != "table:local:accounts,table:local:app_users,table:local:carriers,table:local:credits,"
             "table:local:invoices,table:local:items,table:local:ledgers,table:local:members,table:local:orders,"
             "table:local:parcels,table:local:payments,table:local:refunds,table:local:shipments") {
    return fail("per-language table ids: [" + ids + "]");
  }
  const std::vector<std::pair<std::string, std::string>> users = {
      {"orders", "listOrders"}, {"items", "fromTemplate"}, {"accounts", "listAccounts"}, {"invoices", "load"},
      {"ledgers", "tpl"},       {"app_users", "native"},   {"payments", "all"},          {"shipments", "plus"},
      {"members", "filtered"},  {"credits", "filtered"},   {"refunds", "refunds"},       {"parcels", "where"},
      {"carriers", "where_plus"},
  };
  for (const auto& [table, user] : users) {
    if (const auto got = joined(sides(graph, "table:local:" + table, "CONSUMES")); got != user) {
      return fail(table + " users: [" + got + "], want [" + user + "]");
    }
  }
  return 0;
}

}  // namespace

int main() {
  for (const auto test : {test_sql_text_tables, test_cypher_text_labels, test_test_paths, test_tables_in_one_repo,
                          test_mirrored_drizzle_schema, test_graph_labels, test_sql_strings_per_language}) {
    if (const int status = test(); status != 0) {
      return status;
    }
  }
  std::cout << "data_contracts_test: ok\n";
  return 0;
}
