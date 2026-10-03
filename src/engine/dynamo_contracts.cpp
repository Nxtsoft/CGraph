#include "cgraph/dynamo_contracts.hpp"

#include "cgraph/data_contracts.hpp"
#include "cgraph/env_contracts.hpp"
#include "cgraph/javascript_syntax.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cgraph {
namespace {

using js_syntax::node_text;
using js_syntax::unwrap_expression;

constexpr std::string_view kProvides = "provides_contract";
constexpr std::string_view kUses = "uses_contract";

// v3 command classes (client-dynamodb, lib-dynamodb) and DocumentClient /
// DynamoDBDocument / v2 client methods, by whether they write the table.
constexpr std::array<std::string_view, 7> kWriteCommands = {
    "PutItemCommand", "UpdateItemCommand", "DeleteItemCommand", "CreateTableCommand",
    "PutCommand",     "UpdateCommand",     "DeleteCommand",
};
constexpr std::array<std::string_view, 4> kReadCommands = {"GetItemCommand", "QueryCommand", "ScanCommand",
                                                            "GetCommand"};
constexpr std::array<std::string_view, 7> kWriteMethods = {"put",     "update",     "delete",     "putItem",
                                                           "updateItem", "deleteItem", "createTable"};
constexpr std::array<std::string_view, 4> kReadMethods = {"get", "query", "scan", "getItem"};
// v3 paginators: `paginateQuery({ client }, { TableName, ... })` reads.
constexpr std::array<std::string_view, 2> kReadPaginators = {"paginateQuery", "paginateScan"};
constexpr std::array<std::string_view, 4> kSdkModules = {"@aws-sdk/client-dynamodb", "@aws-sdk/lib-dynamodb",
                                                         "aws-sdk/clients/dynamodb", "aws-sdk"};

[[nodiscard]] std::string_view type_of(const TSNode& node) {
  return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
}
[[nodiscard]] TSNode field(const TSNode& node, std::string_view name) {
  return ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
}
template <std::size_t N>
[[nodiscard]] bool listed(const std::array<std::string_view, N>& names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

// A string literal's text, nullopt for anything else (a template with a
// substitution included).
[[nodiscard]] std::optional<std::string> literal_text(const TSNode& node, std::string_view source) {
  const auto type = type_of(node);
  if (type == "string") {
    return js_syntax::strip_string_quotes(node_text(node, source));
  }
  if (type == "template_string") {
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (type_of(ts_node_named_child(node, i)) == "template_substitution") {
        return std::nullopt;
      }
    }
    return js_syntax::strip_string_quotes(node_text(node, source));
  }
  return std::nullopt;
}

// `process.env.X` / `process.env['X']`: X, else empty.
[[nodiscard]] std::string env_read(const TSNode& node, std::string_view source) {
  const auto type = type_of(node);
  if (type != "member_expression" && type != "subscript_expression") {
    return {};
  }
  const TSNode object = unwrap_expression(field(node, "object"));
  if (type_of(object) != "member_expression" || node_text(object, source) != "process.env") {
    return {};
  }
  std::string name;
  if (type == "member_expression") {
    name = node_text(field(node, "property"), source);
  } else if (auto index = literal_text(unwrap_expression(field(node, "index")), source)) {
    name = std::move(*index);
  }
  return is_env_variable_name(name) ? name : std::string{};
}

struct TableName {
  std::string name;
  std::string env;  // the env variables the name is the default of, comma-separated
};

// A literal, or `process.env.X || ... || 'literal'` (`??` alike).
[[nodiscard]] std::optional<TableName> name_value(TSNode node, std::string_view source) {
  node = unwrap_expression(node);
  if (auto literal = literal_text(node, source)) {
    return TableName{.name = std::move(*literal), .env = {}};
  }
  if (type_of(node) != "binary_expression") {
    return std::nullopt;
  }
  // Flatten the left-associated chain: `(a || b) || 'x'`.
  std::vector<TSNode> operands;
  TSNode current = node;
  while (type_of(current) == "binary_expression") {
    const auto op = node_text(field(current, "operator"), source);
    if (op != "||" && op != "??") {
      return std::nullopt;
    }
    operands.push_back(unwrap_expression(field(current, "right")));
    current = unwrap_expression(field(current, "left"));
  }
  operands.push_back(current);
  std::ranges::reverse(operands);
  auto literal = literal_text(operands.back(), source);
  if (!literal) {
    return std::nullopt;
  }
  TableName out{.name = std::move(*literal), .env = {}};
  for (std::size_t i = 0; i + 1 < operands.size(); ++i) {
    auto env = env_read(operands[i], source);
    if (env.empty()) {
      return std::nullopt;  // `config?.tableName || 'x'`: the name is not the default
    }
    out.env += (out.env.empty() ? "" : ",") + env;
  }
  return out;
}

// The file's DynamoDB imports and module constants.
struct FileIndex {
  bool imports_sdk = false;
  std::unordered_map<std::string, TSNode> constants;  // module-level `const` name -> its value
};

[[nodiscard]] bool is_sdk_module(const TSNode& node, std::string_view source) {
  const auto module = literal_text(node, source);
  return module && listed(kSdkModules, *module);
}

[[nodiscard]] FileIndex index_file(TSNode root, std::string_view source) {
  FileIndex index;
  std::vector<TSNode> stack{root};
  while (!stack.empty()) {
    const TSNode node = stack.back();
    stack.pop_back();
    const auto type = type_of(node);
    if (type == "import_statement" && is_sdk_module(field(node, "source"), source)) {
      index.imports_sdk = true;
    } else if (type == "call_expression") {
      // `require('@aws-sdk/client-dynamodb')`, `await import('@aws-sdk/client-dynamodb')`.
      const auto callee = field(node, "function");
      const auto arguments = field(node, "arguments");
      if ((type_of(callee) == "import" || node_text(callee, source) == "require") &&
          ts_node_named_child_count(arguments) >= 1 && is_sdk_module(ts_node_named_child(arguments, 0), source)) {
        index.imports_sdk = true;
      }
    } else if (type == "lexical_declaration" && js_syntax::is_module_level_declaration(node) &&
               node_text(ts_node_child(node, 0), source) == "const") {
      const auto count = ts_node_named_child_count(node);
      for (std::uint32_t i = 0; i < count; ++i) {
        const TSNode declarator = ts_node_named_child(node, i);
        const TSNode name = field(declarator, "name");
        const TSNode value = field(declarator, "value");
        if (type_of(declarator) == "variable_declarator" && type_of(name) == "identifier" && !ts_node_is_null(value)) {
          index.constants.emplace(node_text(name, source), value);
        }
      }
    }
    const auto count = ts_node_child_count(node);
    for (std::uint32_t i = count; i > 0; --i) {
      stack.push_back(ts_node_child(node, i - 1));
    }
  }
  return index;
}

// The index of the file being extracted, built on its first DynamoDB call.
thread_local std::optional<FileIndex>* current_index = nullptr;

// The file's root, which index_file reads. Done only for a call that already
// passes TableName and a DynamoDB command or method, so a file pays for it only
// where it talks to DynamoDB.
[[nodiscard]] TSNode root_of(TSNode node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    node = parent;
  }
  return node;
}

// Whether a binding pattern (a parameter list, a declarator's name) binds
// `name`: an identifier anywhere in it, destructured (`({ TABLE })`,
// `[TABLE]`, `{ a: TABLE }`) or not. An identifier in a default value
// (`{ a = TABLE }`) counts too, which can only make a read record nothing.
[[nodiscard]] bool binds(const TSNode& pattern, const std::string& name, std::string_view source) {
  std::vector<TSNode> stack{pattern};
  while (!stack.empty()) {
    const TSNode node = stack.back();
    stack.pop_back();
    if (ts_node_is_null(node)) {
      continue;
    }
    const auto type = type_of(node);
    if ((type == "identifier" || type == "shorthand_property_identifier_pattern") && node_text(node, source) == name) {
      return true;
    }
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < count; ++i) {
      stack.push_back(ts_node_named_child(node, i));
    }
  }
  return false;
}

// Whether a parameter or local of a scope enclosing `node` is named `name`.
[[nodiscard]] bool shadowed(const TSNode& node, const std::string& name, std::string_view source) {
  for (TSNode scope = ts_node_parent(node); !ts_node_is_null(scope); scope = ts_node_parent(scope)) {
    const auto type = type_of(scope);
    if (js_syntax::is_function_node(type)) {
      if (binds(field(scope, "parameters"), name, source) || binds(field(scope, "parameter"), name, source)) {
        return true;
      }
    } else if (type == "statement_block") {
      const auto count = ts_node_named_child_count(scope);
      for (std::uint32_t i = 0; i < count; ++i) {
        const TSNode statement = ts_node_named_child(scope, i);
        if (type_of(statement) != "lexical_declaration" && type_of(statement) != "variable_declaration") {
          continue;
        }
        const auto declarators = ts_node_named_child_count(statement);
        for (std::uint32_t j = 0; j < declarators; ++j) {
          if (binds(field(ts_node_named_child(statement, j), "name"), name, source)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

// The `TableName` property's value in an object literal, null when absent.
[[nodiscard]] TSNode table_name_value(const TSNode& object, std::string_view source) {
  if (type_of(object) != "object") {
    return TSNode{};
  }
  const auto count = ts_node_named_child_count(object);
  for (std::uint32_t i = 0; i < count; ++i) {
    const TSNode member = ts_node_named_child(object, i);
    if (type_of(member) == "pair") {
      const TSNode key = field(member, "key");
      const auto spelled = type_of(key) == "property_identifier" ? std::optional(node_text(key, source))
                                                                 : literal_text(key, source);
      if (spelled == "TableName") {
        return field(member, "value");
      }
    } else if (type_of(member) == "shorthand_property_identifier" && node_text(member, source) == "TableName") {
      return member;  // `{ TableName }`: the identifier is its own value
    }
  }
  return TSNode{};
}

[[nodiscard]] std::optional<TableName> resolve_table_name(const TSNode& value, const FileIndex& index,
                                                          std::string_view source) {
  if (auto direct = name_value(value, source)) {
    return direct;
  }
  const TSNode unwrapped = unwrap_expression(value);
  const auto type = type_of(unwrapped);
  if (type != "identifier" && type != "shorthand_property_identifier") {
    return std::nullopt;
  }
  const auto name = node_text(unwrapped, source);
  const auto constant = index.constants.find(name);
  if (constant == index.constants.end() || shadowed(unwrapped, name, source)) {
    return std::nullopt;
  }
  return name_value(constant->second, source);
}

}  // namespace

struct DynamoContractsFileScope::Index {
  std::optional<FileIndex> file;
  std::optional<FileIndex>* previous = nullptr;
};

DynamoContractsFileScope::DynamoContractsFileScope() : index_(std::make_unique<Index>()) {
  index_->previous = current_index;
  current_index = &index_->file;
}

DynamoContractsFileScope::~DynamoContractsFileScope() { current_index = index_->previous; }

bool is_dynamo_table_name(std::string_view name) {
  return name.size() >= 3 && name.size() <= 255 && std::ranges::all_of(name, [](char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                  ch == '-' || ch == '.';
         });
}

void js_dynamo_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         const Fragment& fragment, std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if (type != "new_expression" && type != "call_expression") {
    return;
  }
  const auto source = context.source;
  const TSNode arguments = field(node, "arguments");
  if (type_of(arguments) != "arguments" || ts_node_named_child_count(arguments) == 0) {
    return;
  }
  // The operation: a command class (`PutItemCommand`, or `ddb.PutItemCommand`
  // off a namespace import), a paginator function or a client method.
  const TSNode callee = unwrap_expression(field(node, type == "new_expression" ? "constructor" : "function"));
  const auto operation = type_of(callee) == "member_expression" ? node_text(field(callee, "property"), source)
                                                                 : node_text(callee, source);
  bool writes = false;
  std::uint32_t input = 0;  // the argument holding `TableName`
  if (type == "new_expression") {
    if (!listed(kWriteCommands, operation) && !listed(kReadCommands, operation)) {
      return;
    }
    writes = listed(kWriteCommands, operation);
  } else if (listed(kReadPaginators, operation)) {
    input = 1;  // `paginateQuery({ client }, { TableName })`
  } else {
    if (type_of(callee) != "member_expression" || (!listed(kWriteMethods, operation) && !listed(kReadMethods, operation))) {
      return;
    }
    writes = listed(kWriteMethods, operation);
  }
  if (ts_node_named_child_count(arguments) <= input) {
    return;
  }
  const TSNode value = table_name_value(unwrap_expression(ts_node_named_child(arguments, input)), source);
  if (ts_node_is_null(value)) {
    return;
  }
  if (is_test_source_path(context.relative_path)) {
    return;  // a test's table is a fixture
  }
  std::optional<FileIndex> scratch;  // no file scope: read the file for this call alone
  auto& slot = current_index != nullptr ? *current_index : scratch;
  if (!slot) {
    slot = index_file(root_of(node), source);
  }
  const auto& index = *slot;
  if (!index.imports_sdk) {
    return;
  }
  auto table = resolve_table_name(value, index, source);
  if (!table || !is_dynamo_table_name(table->name)) {
    return;
  }
  auto scope = js_syntax::reading_scope_id(node, context, function_scope_id, fragment);
  const auto relation = writes ? kProvides : kUses;
  const auto fact = "dynamo:" + table->name;
  const bool seen = std::ranges::any_of(out, [&](const RawRelation& existing) {
    return existing.relation == relation && existing.source_id == scope && existing.context == fact &&
           existing.target_label == table->env;
  });
  if (seen) {
    return;
  }
  out.push_back(RawRelation{
      .source_id = std::move(scope),
      .target_label = std::move(table->env),
      .relation = std::string(relation),
      .context = fact,
      .source_file = context.source_file,
  });
}

}  // namespace cgraph
