#include "cgraph/non_grammar_extractors.hpp"

#include "cgraph/contract_schemas.hpp"
#include "cgraph/normalize.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>

namespace cgraph {
namespace {

// A SQL identifier as written in DDL: a double-quoted name or a bare unquoted name.
// Captured WITH any surrounding quotes so normalize_sql_ident can apply the right
// case rule. `$` is legal in PostgreSQL identifiers after the first character.
constexpr const char* kSqlIdent = R"(("[^"]+"|[A-Za-z_][A-Za-z0-9_$]*))";
// Same, non-capturing -- for positions we match but do not key on (old rename name,
// constraint name, schema qualifier).
constexpr const char* kSqlIdentNc = R"((?:"[^"]+"|[A-Za-z_][A-Za-z0-9_$]*))";
// An optional `schema.` qualifier (quoted or unquoted), discarded so ids key on the
// table component -- `"public"."Users"` and `public.users` key on the table.
constexpr const char* kSqlQualifier =
    R"((?:(?:"[^"]+"|[A-Za-z_][A-Za-z0-9_$]*)\s*\.\s*)?)";

// Resolve a raw DDL identifier token to its canonical name: strip the surrounding
// quotes of a quoted identifier (case preserved); fold an unquoted identifier to
// lowercase per PostgreSQL. This canonicalizes the node LABEL; node ids go on through
// make_id, which case-folds (the Graphify id contract), so quoted and unquoted
// references to the same table already reconcile to one id -- case-variant identifiers
// are therefore one node, a deliberate parity property of the id scheme.
[[nodiscard]] std::string normalize_sql_ident(const std::string& raw) {
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    return raw.substr(1, raw.size() - 2);
  }
  std::string folded = raw;
  std::transform(folded.begin(), folded.end(), folded.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return folded;
}

[[nodiscard]] SourceLocation line_location(std::string_view source, std::size_t offset) {
  std::uint32_t line = 1;
  std::uint32_t column = 0;
  for (std::size_t index = 0; index < offset && index < source.size(); ++index) {
    if (source[index] == '\n') {
      ++line;
      column = 0;
    } else {
      ++column;
    }
  }
  return SourceLocation{
      .start_line = line,
      .start_column = column,
      .end_line = line,
      .end_column = column,
  };
}

void add_node(
    Fragment& fragment,
    const ExtractionContext& context,
    std::string label,
    std::string kind,
    std::size_t offset = 0) {
  if (label.empty()) {
    return;
  }

  fragment.nodes.push_back(Node{
      .id = make_id(context.relative_path + ":" + kind + ":" + label),
      .label = std::move(label),
      .source_file = context.source_file,
      .source_location = line_location(context.source, offset),
      .kind = std::move(kind),
      .confidence = Confidence::Extracted,
  });
}

void add_regex_matches(
    Fragment& fragment,
    const ExtractionContext& context,
    const std::regex& pattern,
    std::string kind) {
  const auto begin = std::cregex_iterator(context.source.data(), context.source.data() + context.source.size(), pattern);
  const auto end = std::cregex_iterator();
  for (auto iter = begin; iter != end; ++iter) {
    const auto match = *iter;
    if (match.size() > 1) {
      add_node(fragment, context, match[1].str(), kind, static_cast<std::size_t>(match.position(1)));
    }
  }
}

}  // namespace

ExtractionResult extract_msbuild(const ExtractionContext& context) {
  ExtractionResult result;
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(<\s*(Project|Target|ItemGroup|PropertyGroup)\b[^>]*>)", std::regex::icase},
      "msbuild_element");
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(<\s*Target\b[^>]*\bName\s*=\s*["']([^"']+)["'])", std::regex::icase},
      "target");
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(<\s*PackageReference\b[^>]*\bInclude\s*=\s*["']([^"']+)["'])", std::regex::icase},
      "package_reference");
  return result;
}

ExtractionResult extract_delphi_form(const ExtractionContext& context) {
  ExtractionResult result;
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"((?:object|inherited)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:)", std::regex::icase},
      "form_object");
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(\bprocedure\s+([A-Za-z_][A-Za-z0-9_.]*)\s*\()", std::regex::icase},
      "procedure");
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(\bfunction\s+([A-Za-z_][A-Za-z0-9_.]*)\s*\()", std::regex::icase},
      "function");
  return result;
}

ExtractionResult extract_apex(const ExtractionContext& context) {
  ExtractionResult result;
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(\b(?:public|private|global|protected)?\s*(?:with\s+sharing|without\s+sharing|virtual|abstract)?\s*class\s+([A-Za-z_][A-Za-z0-9_]*)\b)"},
      "class");
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(\btrigger\s+([A-Za-z_][A-Za-z0-9_]*)\s+on\s+([A-Za-z_][A-Za-z0-9_]*)\b)"},
      "trigger");
  add_regex_matches(
      result.fragment,
      context,
      std::regex{R"(\b(?:public|private|global|protected|static|virtual|override|\s)+\s+[A-Za-z_][A-Za-z0-9_<>,\[\]]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"},
      "method");
  return result;
}

ExtractionResult extract_mcp_config(const ExtractionContext& context) {
  ExtractionResult result;
  try {
    const auto json = nlohmann::json::parse(context.source);
    const auto servers = json.find("mcpServers");
    if (servers != json.end() && servers->is_object()) {
      for (const auto& [name, server] : servers->items()) {
        add_node(result.fragment, context, name, "mcp_server");
        if (server.is_object()) {
          if (const auto command = server.find("command"); command != server.end() && command->is_string()) {
            add_node(result.fragment, context, command->get<std::string>(), "mcp_command");
          }
        }
      }
    }
  } catch (const nlohmann::json::exception& error) {
    result.fragment.warnings.push_back(std::string{"failed to parse MCP config JSON: "} + error.what());
  }
  return result;
}

// File-level SQL indexing: emit exactly one node representing the .sql file so the
// data layer (Prisma migrations/schema) is discoverable, enrichable, and
// seam-anchorable. SQL contents are not parsed (symbol-level extraction is a
// follow-up); no source_location, no symbols, no edges.
ExtractionResult extract_sql(const ExtractionContext& context) {
  ExtractionResult result;
  auto& fragment = result.fragment;
  const auto& source_file = context.source_file;
  const auto& source = context.source;
  const auto slash = context.relative_path.find_last_of("/\\");
  const std::string filename =
      slash == std::string::npos ? context.relative_path : context.relative_path.substr(slash + 1);
  if (filename.empty()) {
    return result;
  }

  // File-level node: the .sql file as a discoverable/enrichable/anchorable entity.
  fragment.nodes.push_back(Node{
      .id = make_id(context.relative_path + ":sql_file:" + filename),
      .label = filename,
      .source_file = source_file,
      .kind = "sql_file",
      .confidence = Confidence::Extracted,
  });

  // Schema entities. Ids are keyed on the entity NAME (not the file path) so the
  // same table/enum across append-only migrations merges to one node in the graph
  // builder; the first CREATE defines the node (source_file/line), later ALTERs add
  // only edges.
  const auto add_entity =
      [&](const std::regex& pattern, std::string kind, const std::string& id_prefix) {
        const auto end = std::cregex_iterator();
        for (auto it = std::cregex_iterator(source.data(), source.data() + source.size(), pattern);
             it != end; ++it) {
          const auto& match = *it;
          std::string name = normalize_sql_ident(match[1].str());
          if (name.empty()) {
            continue;
          }
          fragment.nodes.push_back(Node{
              .id = make_id(id_prefix + name),
              .label = std::move(name),
              .source_file = source_file,
              .source_location = line_location(source, static_cast<std::size_t>(match.position(1))),
              .kind = kind,
              .confidence = Confidence::Extracted,
          });
        }
      };
  // Identifiers may be quoted or unquoted, and optionally `schema.`-qualified. The
  // qualifier is skipped so `"public"."Users"` / `public.users` key on the table; the
  // captured token keeps its quotes so normalize_sql_ident applies PostgreSQL case
  // folding (quoted preserves case, unquoted lowercases) -- this is what lets a quoted
  // definition and an unquoted reference of the same table reconcile to one node id.
  const std::string ident = kSqlIdent;
  const std::string ident_nc = kSqlIdentNc;
  const std::string qual = kSqlQualifier;
  add_entity(
      std::regex{R"(CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?)" + qual + ident, std::regex::icase},
      "sql_table", "sql_table:");
  add_entity(std::regex{R"(CREATE\s+TYPE\s+)" + qual + ident + R"(\s+AS\s+ENUM)", std::regex::icase},
             "sql_enum", "sql_enum:");
  // `ALTER TABLE old RENAME TO new` mints a new table identity the later schema (and
  // its foreign keys) references; key on the NEW name so those refs resolve instead of
  // dangling. The old name keeps its own CREATE-defined node.
  add_entity(std::regex{R"(ALTER\s+TABLE\s+)" + qual + ident_nc + R"(\s+RENAME\s+TO\s+)" + qual +
                            ident,
                        std::regex::icase},
             "sql_table", "sql_table:");

  // Foreign keys -> table->table `references` edges (reusing the existing relation so
  // impact / typed explain / query routing operate over the schema). Matches the
  // Prisma form `ALTER TABLE X ADD [CONSTRAINT c] FOREIGN KEY (...) REFERENCES Y` for
  // quoted and unquoted, optionally schema-qualified, X and Y.
  const std::regex fk{R"(ALTER\s+TABLE\s+)" + qual + ident + R"(\s+ADD\s+(?:CONSTRAINT\s+)" +
                          ident_nc + R"(\s+)?FOREIGN\s+KEY\s*\([^)]*\)\s*REFERENCES\s+)" + qual +
                          ident,
                      std::regex::icase};
  const auto end = std::cregex_iterator();
  for (auto it = std::cregex_iterator(source.data(), source.data() + source.size(), fk); it != end;
       ++it) {
    const auto& match = *it;
    const std::string owner = normalize_sql_ident(match[1].str());
    const std::string target = normalize_sql_ident(match[2].str());
    if (owner.empty() || target.empty()) {
      continue;
    }
    fragment.edges.push_back(Edge{
        .source = make_id("sql_table:" + owner),
        .target = make_id("sql_table:" + target),
        .relation = "references",
        .confidence = Confidence::Extracted,
    });
  }
  return result;
}

std::optional<ExtractionResult> extract_non_grammar_language(
    DetectedLanguage language,
    const ExtractionContext& context) {
  switch (language) {
    case DetectedLanguage::MsBuild:
    case DetectedLanguage::Xml:
      return extract_msbuild(context);
    case DetectedLanguage::Delphi:
      return extract_delphi_form(context);
    case DetectedLanguage::Apex:
      return extract_apex(context);
    case DetectedLanguage::McpConfig:
      return extract_mcp_config(context);
    case DetectedLanguage::Sql:
      return extract_sql(context);
    case DetectedLanguage::OpenApi:
      return extract_openapi_document(context);
    case DetectedLanguage::Protobuf:
      return extract_protobuf(context);
    case DetectedLanguage::GraphQL:
      return extract_graphql_sdl(context);
    default:
      return std::nullopt;
  }
}

// Keep in sync with the extract_non_grammar_language dispatch above.
bool handles_non_grammar_language(DetectedLanguage language) {
  switch (language) {
    case DetectedLanguage::MsBuild:
    case DetectedLanguage::Xml:
    case DetectedLanguage::Delphi:
    case DetectedLanguage::Apex:
    case DetectedLanguage::McpConfig:
    case DetectedLanguage::Sql:
    case DetectedLanguage::OpenApi:
    case DetectedLanguage::Protobuf:
    case DetectedLanguage::GraphQL:
      return true;
    default:
      return false;
  }
}

}  // namespace cgraph
