#include "cgraph/non_grammar_extractors.hpp"

int main() {
  const auto msbuild = cgraph::extract_msbuild(cgraph::ExtractionContext{
      .source_file = "app.csproj",
      .relative_path = "app.csproj",
      .source = R"xml(<Project><Target Name="Build"/><PackageReference Include="Newtonsoft.Json"/></Project>)xml",
  });
  if (msbuild.fragment.nodes.size() < 3) {
    return 1;
  }

  const auto delphi = cgraph::extract_delphi_form(cgraph::ExtractionContext{
      .source_file = "Form1.dfm",
      .relative_path = "Form1.dfm",
      .source = "object Form1: TForm1\nend\nprocedure TForm1.Save();",
  });
  if (delphi.fragment.nodes.size() < 2) {
    return 1;
  }

  const auto apex = cgraph::extract_apex(cgraph::ExtractionContext{
      .source_file = "AccountTrigger.trigger",
      .relative_path = "AccountTrigger.trigger",
      .source = "trigger AccountTrigger on Account (before insert) {}\npublic class Worker { public void run() {} }",
  });
  if (apex.fragment.nodes.size() < 2) {
    return 1;
  }

  const auto mcp = cgraph::extract_mcp_config(cgraph::ExtractionContext{
      .source_file = "mcp.json",
      .relative_path = "mcp.json",
      .source = R"json({"mcpServers":{"filesystem":{"command":"npx"}}})json",
  });
  if (mcp.fragment.nodes.size() != 2 || !mcp.fragment.warnings.empty()) {
    return 1;
  }

  const auto bad_mcp = cgraph::extract_mcp_config(cgraph::ExtractionContext{
      .source_file = "mcp.json",
      .relative_path = "mcp.json",
      .source = "{",
  });
  if (bad_mcp.fragment.warnings.empty()) {
    return 1;
  }

  // A .sql migration yields: the file-level node, name-keyed table/enum nodes, and
  // a foreign-key `references` edge between the owning and referenced tables.
  const auto find = [](const cgraph::Fragment& f, const std::string& kind,
                       const std::string& label) -> const cgraph::Node* {
    for (const auto& node : f.nodes) {
      if (node.kind == kind && node.label == label) {
        return &node;
      }
    }
    return nullptr;
  };
  const auto sql = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "prisma/migrations/20201214_baseline/migration.sql",
      .relative_path = "prisma/migrations/20201214_baseline/migration.sql",
      .source = "CREATE TYPE \"Role\" AS ENUM ('USER','ADMIN');\n"
                "CREATE TABLE \"Brand\" (id TEXT);\n"
                "CREATE TABLE \"Project\" (id TEXT);\n"
                "ALTER TABLE \"Brand\" ADD FOREIGN KEY(\"projectId\")REFERENCES \"Project\"(\"id\");",
  });
  const auto* file_node = find(sql.fragment, "sql_file", "migration.sql");
  const auto* brand = find(sql.fragment, "sql_table", "Brand");
  const auto* project = find(sql.fragment, "sql_table", "Project");
  const auto* role = find(sql.fragment, "sql_enum", "Role");
  if (file_node == nullptr || brand == nullptr || project == nullptr || role == nullptr) {
    return 1;
  }
  // Exactly one references edge, Brand -> Project, with endpoints matching the table node ids.
  if (sql.fragment.edges.size() != 1 || sql.fragment.edges[0].relation != "references" ||
      sql.fragment.edges[0].source != brand->id || sql.fragment.edges[0].target != project->id) {
    return 1;
  }

  // Name-keyed merge intent: the same table CREATEd in a DIFFERENT file has the same
  // id (path-independent), so the graph builder collapses them to one node.
  const auto sql2 = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "prisma/migrations/20210101_other/migration.sql",
      .relative_path = "prisma/migrations/20210101_other/migration.sql",
      .source = "CREATE TABLE \"Brand\" (id TEXT);",
  });
  const auto* brand2 = find(sql2.fragment, "sql_table", "Brand");
  if (brand2 == nullptr || brand2->id != brand->id) {
    return 1;
  }

  // Schema-qualified refs (`"public"."Project"`) must key on the TABLE, not the
  // schema -- otherwise every FK collapses to a dangling edge to a phantom
  // `sql_table:public`. Owner is qualified, CREATE TABLE is not: the edge endpoints
  // must still match the unqualified table node ids.
  const auto sql3 = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "prisma/migrations/20210202_qualified/migration.sql",
      .relative_path = "prisma/migrations/20210202_qualified/migration.sql",
      .source = "CREATE TABLE \"Brand\" (id TEXT);\n"
                "CREATE TABLE \"Project\" (id TEXT);\n"
                "ALTER TABLE \"public\".\"Brand\" ADD CONSTRAINT \"fk\" "
                "FOREIGN KEY(\"projectId\") REFERENCES \"public\".\"Project\"(\"id\");",
  });
  const auto* brand3 = find(sql3.fragment, "sql_table", "Brand");
  const auto* project3 = find(sql3.fragment, "sql_table", "Project");
  if (brand3 == nullptr || project3 == nullptr) {
    return 1;
  }
  // No phantom `public` table node, and exactly the Brand -> Project edge.
  if (find(sql3.fragment, "sql_table", "public") != nullptr) {
    return 1;
  }
  if (sql3.fragment.edges.size() != 1 || sql3.fragment.edges[0].source != brand3->id ||
      sql3.fragment.edges[0].target != project3->id) {
    return 1;
  }

  // `ALTER TABLE RENAME TO` mints the new table identity so a later FK to the new
  // name resolves; without it the reference dangles to a non-existent node.
  const auto sql4 = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "prisma/migrations/20210303_rename/migration.sql",
      .relative_path = "prisma/migrations/20210303_rename/migration.sql",
      .source = "CREATE TABLE \"Objective\" (id TEXT);\n"
                "ALTER TABLE \"Objective\" RENAME TO \"CategoryObjective\";\n"
                "CREATE TABLE \"Tag\" (id TEXT);\n"
                "ALTER TABLE \"Tag\" ADD FOREIGN KEY(\"objId\") REFERENCES \"CategoryObjective\"(\"id\");",
  });
  const auto* renamed = find(sql4.fragment, "sql_table", "CategoryObjective");
  const auto* tag = find(sql4.fragment, "sql_table", "Tag");
  if (renamed == nullptr || tag == nullptr) {
    return 1;
  }
  // The FK edge targets the renamed-to table node (no dangling endpoint).
  bool found_edge = false;
  for (const auto& edge : sql4.fragment.edges) {
    if (edge.source == tag->id && edge.target == renamed->id) {
      found_edge = true;
    }
  }
  if (!found_edge) {
    return 1;
  }

  // Unquoted DDL: tables, enums, and FKs declared without double quotes are
  // extracted just like the quoted form (Postgres folds unquoted names to lower).
  const auto sql5 = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "db/migrations/0050_unquoted.sql",
      .relative_path = "db/migrations/0050_unquoted.sql",
      .source = "CREATE TYPE status AS ENUM ('on','off');\n"
                "CREATE TABLE IF NOT EXISTS skills (id UUID);\n"
                "CREATE TABLE IF NOT EXISTS organizations (id UUID);\n"
                "ALTER TABLE skills ADD CONSTRAINT skills_org_fk "
                "FOREIGN KEY (org_id) REFERENCES organizations(id);",
  });
  const auto* skills = find(sql5.fragment, "sql_table", "skills");
  const auto* orgs = find(sql5.fragment, "sql_table", "organizations");
  const auto* status_enum = find(sql5.fragment, "sql_enum", "status");
  if (skills == nullptr || orgs == nullptr || status_enum == nullptr) {
    return 1;
  }
  // `IF NOT EXISTS` must not be captured as a table name.
  if (find(sql5.fragment, "sql_table", "if") != nullptr ||
      find(sql5.fragment, "sql_table", "exists") != nullptr) {
    return 1;
  }
  if (sql5.fragment.edges.size() != 1 || sql5.fragment.edges[0].source != skills->id ||
      sql5.fragment.edges[0].target != orgs->id) {
    return 1;
  }

  // Mixed reconciliation: a table defined unquoted is referenced quoted (and vice
  // versa); both resolve to the single existing node id, no dangling endpoint.
  const auto sql6 = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "db/migrations/0051_mixed.sql",
      .relative_path = "db/migrations/0051_mixed.sql",
      .source = "CREATE TABLE accounts (id UUID);\n"          // unquoted def
                "CREATE TABLE \"sessions\" (id UUID);\n"      // quoted def
                "ALTER TABLE sessions ADD FOREIGN KEY (acct) REFERENCES \"accounts\"(id);\n"
                "ALTER TABLE \"accounts\" ADD FOREIGN KEY (sess) REFERENCES sessions(id);",
  });
  const auto* accounts = find(sql6.fragment, "sql_table", "accounts");
  const auto* sessions = find(sql6.fragment, "sql_table", "sessions");
  if (accounts == nullptr || sessions == nullptr) {
    return 1;
  }
  // Exactly the two FK edges, both with endpoints among the two real node ids.
  if (sql6.fragment.edges.size() != 2) {
    return 1;
  }
  for (const auto& edge : sql6.fragment.edges) {
    const bool ok = (edge.source == sessions->id && edge.target == accounts->id) ||
                    (edge.source == accounts->id && edge.target == sessions->id);
    if (!ok) {
      return 1;
    }
  }

  // Case reconciliation: node ids are case-folded by make_id (the Graphify id
  // contract), so a quoted "Users" reference and an unquoted users definition resolve
  // to the SAME node id -- the FK does not dangle. (Case-variant identifiers cannot be
  // distinct nodes under this contract; that is a deliberate parity property, not a
  // SQL-extractor concern.)
  const auto sql7 = cgraph::extract_sql(cgraph::ExtractionContext{
      .source_file = "db/migrations/0052_case.sql",
      .relative_path = "db/migrations/0052_case.sql",
      .source = "CREATE TABLE users (id UUID);\n"
                "CREATE TABLE refs (id UUID);\n"
                "ALTER TABLE refs ADD FOREIGN KEY (u) REFERENCES \"Users\"(id);",
  });
  const auto* users_def = find(sql7.fragment, "sql_table", "users");
  const auto* refs = find(sql7.fragment, "sql_table", "refs");
  if (users_def == nullptr || refs == nullptr) {
    return 1;
  }
  if (sql7.fragment.edges.size() != 1 || sql7.fragment.edges[0].source != refs->id ||
      sql7.fragment.edges[0].target != users_def->id) {
    return 1;
  }

  return 0;
}
