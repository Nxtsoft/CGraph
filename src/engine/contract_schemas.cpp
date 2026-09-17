#include "cgraph/contract_schemas.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/normalize.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

constexpr std::array<std::string_view, 8> kOpenApiMethods = {
    "get", "put", "post", "delete", "options", "head", "patch", "trace",
};

[[nodiscard]] std::string to_upper(std::string_view text) {
  std::string upper(text);
  for (auto& ch : upper) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return upper;
}

// Line numbers for byte offsets: newline offsets once, then a binary search.
class LineIndex {
 public:
  explicit LineIndex(std::string_view source) {
    for (std::size_t index = 0; index < source.size(); ++index) {
      if (source[index] == '\n') {
        newlines_.push_back(index);
      }
    }
  }
  [[nodiscard]] SourceLocation at(std::size_t offset) const {
    // Newlines strictly before the offset: their count is the zero-based line.
    const auto before = static_cast<std::size_t>(std::lower_bound(newlines_.begin(), newlines_.end(), offset) - newlines_.begin());
    const std::size_t line_start = before == 0 ? 0 : newlines_[before - 1] + 1;
    const auto column = static_cast<std::uint32_t>(offset >= line_start ? offset - line_start : 0);
    return SourceLocation{
        .start_line = static_cast<std::uint32_t>(before + 1),
        .start_column = column,
        .end_line = static_cast<std::uint32_t>(before + 1),
        .end_column = column,
    };
  }

 private:
  std::vector<std::size_t> newlines_;
};

// The one place a contract document's nodes and edges are shaped: file node,
// symbols with `contains`, endpoints with their canonical ids, fields through
// the shared emitter, edges deduplicated.
class Emitter {
 public:
  Emitter(const ExtractionContext& context, Fragment& fragment)
      : context_(context), fragment_(fragment), lines_(context.source), file_id_(make_id(context.source_file)) {
    const std::filesystem::path source_path(context.source_file);
    std::string file_label = source_path.filename().string();
    if (source_path.has_parent_path() && source_path.parent_path().has_filename()) {
      file_label = source_path.parent_path().filename().string() + "/" + file_label;
    }
    fragment_.nodes.push_back(Node{
        .id = file_id_,
        .label = file_label.empty() ? context.source_file : std::move(file_label),
        .source_file = context.source_file,
        .source_location = SourceLocation{.start_line = 1, .end_line = 1},
        .kind = "file",
        .confidence = Confidence::Extracted,
    });
    node_ids_.insert(file_id_);
  }

  [[nodiscard]] const std::string& file_id() const { return file_id_; }
  [[nodiscard]] const LineIndex& lines() const { return lines_; }

  // A declared symbol of the document: `schema`, or the `type` a proto service is.
  std::string symbol(const std::string& label, std::string_view kind, std::size_t offset, Properties properties) {
    auto id = make_id(context_.source_file + ":" + std::string(kind) + ":" + label);
    if (node_ids_.insert(id).second) {
      fragment_.nodes.push_back(Node{
          .id = id,
          .label = label,
          .source_file = context_.source_file,
          .source_location = lines_.at(offset),
          .kind = std::string(kind),
          .confidence = Confidence::Extracted,
          .properties = std::move(properties),
      });
      edge(file_id_, id, "contains");
    }
    return id;
  }

  // A documented endpoint: the canonical contract id, the document's spelling
  // as label and `path`.
  std::string endpoint(const std::string& method, const std::string& path, std::size_t offset, Properties properties) {
    auto id = "endpoint:" + method + " " + canonical_route_path(path);
    if (node_ids_.insert(id).second) {
      properties.emplace("method", method);
      properties.emplace("path", path);
      properties.emplace("documented", "true");
      fragment_.nodes.push_back(Node{
          .id = id,
          .label = method + " " + path,
          .source_file = context_.source_file,
          .source_location = lines_.at(offset),
          .kind = "endpoint",
          .confidence = Confidence::Extracted,
          .properties = std::move(properties),
      });
      edge(file_id_, id, "contains");
    }
    return id;
  }

  void field(const std::string& owner_id, std::string_view owner_label, std::string name, std::size_t offset,
             Properties properties) {
    add_field_node(context_, owner_id, owner_label, std::move(name), lines_.at(offset), std::move(properties), fragment_);
  }

  void edge(const std::string& source, const std::string& target, std::string_view relation) {
    if (source.empty() || target.empty() || source == target) {
      return;
    }
    if (edge_keys_.insert(source + '\n' + std::string(relation) + '\n' + target).second) {
      fragment_.edges.push_back(Edge{
          .source = source,
          .target = target,
          .relation = std::string(relation),
          .confidence = Confidence::Extracted,
      });
    }
  }

 private:
  const ExtractionContext& context_;
  Fragment& fragment_;
  LineIndex lines_;
  std::string file_id_;
  std::unordered_set<std::string> node_ids_;
  std::unordered_set<std::string> edge_keys_;
};

// ---- OpenAPI (JSON) --------------------------------------------------------

// The byte offset of `"key":` in the document, or 0. nlohmann parses without
// positions; the first spelling of a key as a property name is the declaration.
[[nodiscard]] std::size_t key_offset(std::string_view source, const std::string& key) {
  const auto quoted = "\"" + key + "\"";
  std::size_t from = 0;
  while (from < source.size()) {
    const auto at = source.find(quoted, from);
    if (at == std::string_view::npos) {
      return 0;
    }
    auto after = at + quoted.size();
    while (after < source.size() && (source[after] == ' ' || source[after] == '\t')) {
      ++after;
    }
    if (after < source.size() && source[after] == ':') {
      return at;
    }
    from = at + 1;
  }
  return 0;
}

// The schema a JSON Schema fragment names: its `$ref` under `prefix`, or its
// array items' `$ref`. Empty for an inline or primitive schema.
[[nodiscard]] std::string ref_name(const nlohmann::json& schema, std::string_view prefix) {
  if (!schema.is_object()) {
    return {};
  }
  if (const auto ref = schema.find("$ref"); ref != schema.end() && ref->is_string()) {
    const auto text = ref->get<std::string>();
    return text.starts_with(prefix) ? text.substr(prefix.size()) : std::string{};
  }
  if (const auto items = schema.find("items"); items != schema.end()) {
    return ref_name(*items, prefix);
  }
  return {};
}

[[nodiscard]] std::string schema_type_text(const nlohmann::json& schema, std::string_view prefix) {
  if (!schema.is_object()) {
    return {};
  }
  if (const auto name = ref_name(schema, prefix); !name.empty()) {
    return schema.contains("items") ? name + "[]" : name;
  }
  if (const auto type = schema.find("type"); type != schema.end() && type->is_string()) {
    auto text = type->get<std::string>();
    if (text == "array") {
      if (const auto items = schema.find("items"); items != schema.end()) {
        const auto inner = schema_type_text(*items, prefix);
        return (inner.empty() ? std::string{"unknown"} : inner) + "[]";
      }
    }
    if (const auto format = schema.find("format"); format != schema.end() && format->is_string()) {
      text += " (" + format->get<std::string>() + ")";
    }
    return text;
  }
  if (schema.contains("enum")) {
    return "enum";
  }
  return {};
}

}  // namespace

ExtractionResult extract_openapi_document(const ExtractionContext& context) {
  ExtractionResult result;
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(context.source);
  } catch (const nlohmann::json::exception& error) {
    result.fragment.warnings.push_back(std::string{"failed to parse OpenAPI JSON: "} + error.what());
    return result;
  }
  if (!doc.is_object() || (!doc.contains("openapi") && !doc.contains("swagger"))) {
    result.fragment.warnings.push_back("not an OpenAPI document (no `openapi` or `swagger` key): " + context.source_file);
    return result;
  }
  const bool swagger2 = !doc.contains("openapi");
  const std::string prefix = swagger2 ? "#/definitions/" : "#/components/schemas/";
  Emitter emit(context, result.fragment);

  const nlohmann::json* schemas = nullptr;
  if (swagger2) {
    if (const auto defs = doc.find("definitions"); defs != doc.end() && defs->is_object()) {
      schemas = &*defs;
    }
  } else if (const auto components = doc.find("components"); components != doc.end() && components->is_object()) {
    if (const auto found = components->find("schemas"); found != components->end() && found->is_object()) {
      schemas = &*found;
    }
  }

  // Schemas first, so endpoint and property references have targets.
  std::unordered_map<std::string, std::string> schema_ids;
  if (schemas != nullptr) {
    for (const auto& [name, schema] : schemas->items()) {
      schema_ids.emplace(name, emit.symbol(name, "schema", key_offset(context.source, name), {{"format", "openapi"}}));
    }
    for (const auto& [name, schema] : schemas->items()) {
      const auto& owner = schema_ids.at(name);
      std::unordered_set<std::string> required;
      if (const auto list = schema.find("required"); list != schema.end() && list->is_array()) {
        for (const auto& entry : *list) {
          if (entry.is_string()) {
            required.insert(entry.get<std::string>());
          }
        }
      }
      const auto declared_at = key_offset(context.source, name);
      if (const auto properties = schema.find("properties"); properties != schema.end() && properties->is_object()) {
        for (const auto& [property, spec] : properties->items()) {
          Properties field_properties;
          if (auto type_text = schema_type_text(spec, prefix); !type_text.empty()) {
            field_properties.emplace("type_text", std::move(type_text));
          }
          field_properties.emplace("optional", required.contains(property) ? "false" : "true");
          emit.field(owner, name, property, declared_at, std::move(field_properties));
          if (const auto target = schema_ids.find(ref_name(spec, prefix)); target != schema_ids.end()) {
            emit.edge(owner, target->second, "references");
          }
        }
      }
      for (const auto* combinator : {"allOf", "oneOf", "anyOf"}) {
        if (const auto list = schema.find(combinator); list != schema.end() && list->is_array()) {
          for (const auto& entry : *list) {
            if (const auto target = schema_ids.find(ref_name(entry, prefix)); target != schema_ids.end()) {
              emit.edge(owner, target->second, std::string_view(combinator) == "allOf" ? "inherits" : "references");
            }
          }
        }
      }
    }
  }

  // Paths: one endpoint per path and method, with the 2xx response and request
  // body schemas it names.
  if (const auto paths = doc.find("paths"); paths != doc.end() && paths->is_object()) {
    for (const auto& [path, item] : paths->items()) {
      if (path.starts_with("x-") || !item.is_object()) {
        continue;
      }
      const auto path_at = key_offset(context.source, path);
      for (const auto method : kOpenApiMethods) {
        const auto operation = item.find(std::string(method));
        if (operation == item.end() || !operation->is_object()) {
          continue;
        }
        Properties properties{{"format", "openapi"}};
        if (const auto id = operation->find("operationId"); id != operation->end() && id->is_string()) {
          properties.emplace("operation", id->get<std::string>());
        }
        const auto endpoint = emit.endpoint(to_upper(method), path, path_at, std::move(properties));
        const auto link = [&](const nlohmann::json& schema, std::string_view relation) {
          if (const auto target = schema_ids.find(ref_name(schema, prefix)); target != schema_ids.end()) {
            emit.edge(endpoint, target->second, relation);
          }
        };
        if (const auto responses = operation->find("responses"); responses != operation->end() && responses->is_object()) {
          for (const auto& [status, response] : responses->items()) {
            if (!(status.starts_with('2') || status == "default") || !response.is_object()) {
              continue;
            }
            if (swagger2) {
              if (const auto schema = response.find("schema"); schema != response.end()) {
                link(*schema, "RESPONDS_WITH");
              }
            } else if (const auto content = response.find("content"); content != response.end() && content->is_object()) {
              for (const auto& [media, body] : content->items()) {
                if (const auto schema = body.find("schema"); body.is_object() && schema != body.end()) {
                  link(*schema, "RESPONDS_WITH");
                }
              }
            }
          }
        }
        if (swagger2) {
          if (const auto parameters = operation->find("parameters"); parameters != operation->end() && parameters->is_array()) {
            for (const auto& parameter : *parameters) {
              if (parameter.is_object() && parameter.value("in", std::string{}) == "body" && parameter.contains("schema")) {
                link(parameter["schema"], "ACCEPTS");
              }
            }
          }
        } else if (const auto body = operation->find("requestBody"); body != operation->end() && body->is_object()) {
          if (const auto content = body->find("content"); content != body->end() && content->is_object()) {
            for (const auto& [media, entry] : content->items()) {
              if (const auto schema = entry.find("schema"); entry.is_object() && schema != entry.end()) {
                link(*schema, "ACCEPTS");
              }
            }
          }
        }
      }
    }
  }
  return result;
}

// ---- Protocol Buffers ---------------------------------------------------------

namespace {

struct Token {
  std::string text;
  std::size_t offset = 0;
};

[[nodiscard]] bool is_ident_char(char ch, bool allow_dot) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || (allow_dot && ch == '.');
}

// Tokens of a proto or GraphQL document: identifiers (dotted for proto), numbers,
// strings as one token, single punctuation characters; comments dropped.
[[nodiscard]] std::vector<Token> tokenize(std::string_view source, bool dotted_identifiers, bool hash_comments) {
  std::vector<Token> tokens;
  std::size_t index = 0;
  while (index < source.size()) {
    const char ch = source[index];
    if (std::isspace(static_cast<unsigned char>(ch)) != 0 || (hash_comments && ch == ',')) {
      ++index;
      continue;
    }
    if (hash_comments && ch == '#') {
      while (index < source.size() && source[index] != '\n') {
        ++index;
      }
      continue;
    }
    if (!hash_comments && ch == '/' && index + 1 < source.size() && source[index + 1] == '/') {
      while (index < source.size() && source[index] != '\n') {
        ++index;
      }
      continue;
    }
    if (!hash_comments && ch == '/' && index + 1 < source.size() && source[index + 1] == '*') {
      const auto end = source.find("*/", index + 2);
      index = end == std::string_view::npos ? source.size() : end + 2;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      // A block string `"""..."""` or a quoted string; either is one token.
      if (ch == '"' && source.substr(index, 3) == "\"\"\"") {
        const auto end = source.find("\"\"\"", index + 3);
        const auto stop = end == std::string_view::npos ? source.size() : end + 3;
        tokens.push_back(Token{.text = std::string(source.substr(index, stop - index)), .offset = index});
        index = stop;
        continue;
      }
      std::size_t end = index + 1;
      while (end < source.size() && source[end] != ch) {
        end += source[end] == '\\' ? 2 : 1;
      }
      const auto stop = std::min(end + 1, source.size());
      tokens.push_back(Token{.text = std::string(source.substr(index, stop - index)), .offset = index});
      index = stop;
      continue;
    }
    if (is_ident_char(ch, false) || (dotted_identifiers && ch == '.')) {
      std::size_t end = index;
      while (end < source.size() && is_ident_char(source[end], dotted_identifiers)) {
        ++end;
      }
      tokens.push_back(Token{.text = std::string(source.substr(index, end - index)), .offset = index});
      index = end;
      continue;
    }
    tokens.push_back(Token{.text = std::string(1, ch), .offset = index});
    ++index;
  }
  return tokens;
}

[[nodiscard]] bool is_string_token(const std::string& text) {
  return !text.empty() && (text.front() == '"' || text.front() == '\'');
}

// Skips from an opening brace/paren/bracket at `index` to just past its match.
[[nodiscard]] std::size_t skip_balanced(const std::vector<Token>& tokens, std::size_t index) {
  int depth = 0;
  for (; index < tokens.size(); ++index) {
    const auto& text = tokens[index].text;
    if (text == "{" || text == "(" || text == "[") {
      ++depth;
    } else if (text == "}" || text == ")" || text == "]") {
      if (--depth <= 0) {
        return index + 1;
      }
    }
  }
  return tokens.size();
}

// Skips to just past the next `;` at brace depth zero (an `option` statement
// may carry an aggregate `{ ... }`).
[[nodiscard]] std::size_t skip_statement(const std::vector<Token>& tokens, std::size_t index) {
  int depth = 0;
  for (; index < tokens.size(); ++index) {
    const auto& text = tokens[index].text;
    if (text == "{") {
      ++depth;
    } else if (text == "}") {
      --depth;
    } else if (text == ";" && depth <= 0) {
      return index + 1;
    }
  }
  return tokens.size();
}

struct PendingEdge {
  std::string source;
  std::string target_name;
  std::string relation;
};

// A proto type reference resolved against the file's messages and enums: exact
// qualified name (package stripped), then a unique `.Name` suffix, then a unique
// bare name. Anything else (an imported type, `string`, `int32`) has no target.
[[nodiscard]] std::string resolve_proto_name(const std::unordered_map<std::string, std::string>& schemas,
                                             const std::string& package, std::string name) {
  if (name.starts_with('.')) {
    name.erase(0, 1);
  }
  if (!package.empty() && name.starts_with(package + ".")) {
    name.erase(0, package.size() + 1);
  }
  if (const auto exact = schemas.find(name); exact != schemas.end()) {
    return exact->second;
  }
  std::string found;
  for (const auto& [qualified, id] : schemas) {
    if (qualified.ends_with("." + name)) {
      if (!found.empty()) {
        return {};  // two nested messages share the leaf name
      }
      found = id;
    }
  }
  return found;
}

}  // namespace

ExtractionResult extract_protobuf(const ExtractionContext& context) {
  ExtractionResult result;
  Emitter emit(context, result.fragment);
  const auto tokens = tokenize(context.source, /*dotted_identifiers=*/true, /*hash_comments=*/false);

  struct Scope {
    std::string kind;   // message | enum | service | oneof | block
    std::string label;  // qualified message name, enum name, service name
    std::string id;
  };
  std::vector<Scope> scopes;
  std::string package;
  std::unordered_map<std::string, std::string> schemas;  // qualified name -> id
  std::vector<PendingEdge> pending;

  const auto enclosing_message = [&]() -> const Scope* {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      if (it->kind == "message") {
        return &*it;
      }
    }
    return nullptr;
  };
  const auto qualify = [&](const std::string& name) {
    const auto* outer = enclosing_message();
    return outer == nullptr ? name : outer->label + "." + name;
  };

  std::size_t index = 0;
  while (index < tokens.size()) {
    const auto& token = tokens[index];
    const auto& text = token.text;
    const auto next = [&](std::size_t ahead) -> const std::string& {
      static const std::string empty;
      return index + ahead < tokens.size() ? tokens[index + ahead].text : empty;
    };
    if (text == "package") {
      package.clear();
      std::size_t cursor = index + 1;
      while (cursor < tokens.size() && tokens[cursor].text != ";") {
        package += tokens[cursor++].text;
      }
      index = cursor + 1;
      continue;
    }
    if (text == "syntax" || text == "edition" || text == "import" || text == "option" || text == "reserved" ||
        text == "extensions" || text == "extend") {
      index = text == "extend" && next(2) == "{" ? skip_balanced(tokens, index + 2) : skip_statement(tokens, index);
      continue;
    }
    if (text == "message" || text == "enum") {
      const auto label = qualify(next(1));
      Properties properties{{"format", "protobuf"}};
      if (text == "enum") {
        properties.emplace("enum", "true");
      }
      const auto id = emit.symbol(label, "schema", token.offset, std::move(properties));
      schemas.emplace(label, id);
      scopes.push_back(Scope{.kind = text, .label = label, .id = id});
      index += next(2) == "{" ? 3 : 2;
      continue;
    }
    if (text == "service") {
      const auto id = emit.symbol(next(1), "type", token.offset, {{"format", "protobuf"}, {"protobuf", "service"}});
      scopes.push_back(Scope{.kind = "service", .label = next(1), .id = id});
      index += next(2) == "{" ? 3 : 2;
      continue;
    }
    if (text == "oneof") {
      scopes.push_back(Scope{.kind = "oneof"});
      index += next(2) == "{" ? 3 : 2;
      continue;
    }
    if (text == "{") {
      scopes.push_back(Scope{.kind = "block"});
      ++index;
      continue;
    }
    if (text == "}") {
      if (!scopes.empty()) {
        scopes.pop_back();
      }
      ++index;
      continue;
    }
    if (text == "rpc" && !scopes.empty() && scopes.back().kind == "service") {
      const auto& service = scopes.back();
      const auto method = next(1);
      std::size_t cursor = index + 2;  // "("
      const auto read_type = [&](std::size_t& at) {
        std::string type;
        if (at < tokens.size() && tokens[at].text == "(") {
          ++at;
        }
        if (at < tokens.size() && tokens[at].text == "stream") {
          ++at;
        }
        if (at < tokens.size()) {
          type = tokens[at++].text;
        }
        if (at < tokens.size() && tokens[at].text == ")") {
          ++at;
        }
        return type;
      };
      const auto request = read_type(cursor);
      if (cursor < tokens.size() && tokens[cursor].text == "returns") {
        ++cursor;
      }
      const auto response = read_type(cursor);
      const auto path = "/" + (package.empty() ? std::string{} : package + ".") + service.label + "/" + method;
      const auto endpoint = emit.endpoint("POST", path, token.offset, {{"format", "protobuf"}, {"protocol", "grpc"}});
      emit.edge(service.id, endpoint, "defines");
      pending.push_back(PendingEdge{.source = endpoint, .target_name = request, .relation = "ACCEPTS"});
      pending.push_back(PendingEdge{.source = endpoint, .target_name = response, .relation = "RESPONDS_WITH"});
      index = cursor < tokens.size() && tokens[cursor].text == "{" ? skip_balanced(tokens, cursor) : skip_statement(tokens, cursor);
      continue;
    }
    const Scope* current = scopes.empty() ? nullptr : &scopes.back();
    if (current != nullptr && current->kind == "enum") {
      // `NAME = 0 [deprecated = true];`
      if (next(1) == "=") {
        emit.field(current->id, current->label, text, token.offset, {});
      }
      index = skip_statement(tokens, index);
      continue;
    }
    if (const auto* message = enclosing_message(); message != nullptr && current != nullptr &&
                                                    (current->kind == "message" || current->kind == "oneof")) {
      // `[repeated|optional|required] TYPE NAME = N [options];` or `map<K, V> NAME = N;`
      std::size_t cursor = index;
      std::string label;
      if (text == "repeated" || text == "optional" || text == "required") {
        label = text;
        ++cursor;
      }
      std::string type;
      std::string value_type;
      if (cursor < tokens.size() && tokens[cursor].text == "map") {
        std::size_t end = cursor;
        while (end < tokens.size() && tokens[end].text != ">") {
          type += tokens[end++].text;
        }
        type += ">";
        if (end >= 3 && tokens[end - 1].text != "," ) {
          value_type = tokens[end - 1].text;
        }
        cursor = end + 1;
      } else if (cursor < tokens.size()) {
        type = tokens[cursor++].text;
        value_type = type;
      }
      if (cursor + 1 < tokens.size() && tokens[cursor + 1].text == "=") {
        const auto& name = tokens[cursor].text;
        Properties properties{{"type_text", label.empty() ? type : label + " " + type}};
        properties.emplace("optional", label == "required" ? "false" : "true");
        emit.field(message->id, message->label, name, token.offset, std::move(properties));
        pending.push_back(PendingEdge{.source = message->id, .target_name = value_type, .relation = "references"});
      }
      index = skip_statement(tokens, index);
      continue;
    }
    ++index;
  }

  for (const auto& edge : pending) {
    const auto target = resolve_proto_name(schemas, package, edge.target_name);
    if (!target.empty()) {
      emit.edge(edge.source, target, edge.relation);
    }
  }
  return result;
}

// ---- GraphQL SDL --------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, 5> kGraphqlBuiltins = {"String", "Int", "Float", "Boolean", "ID"};

// Reads a GraphQL type reference (`[Notebook!]!`) at `index`: the raw spelling,
// the named type inside, and whether it is non-null at the top.
struct GraphqlType {
  std::string text;
  std::string name;
  bool non_null = false;
};

[[nodiscard]] GraphqlType read_graphql_type(const std::vector<Token>& tokens, std::size_t& index) {
  GraphqlType type;
  int depth = 0;
  while (index < tokens.size()) {
    const auto& text = tokens[index].text;
    if (text == "[") {
      ++depth;
    } else if (text == "]") {
      --depth;
    } else if (text == "!") {
      if (depth == 0) {
        type.non_null = true;
      }
    } else if (is_ident_char(text.front(), false) && type.name.empty()) {
      type.name = text;
    } else if (depth == 0) {
      break;
    }
    type.text += text;
    ++index;
    if (depth == 0 && !type.name.empty() && (index >= tokens.size() || tokens[index].text != "!")) {
      break;
    }
  }
  return type;
}

// Skips `@name` and `@name(...)` directives at `index`.
void skip_directives(const std::vector<Token>& tokens, std::size_t& index) {
  while (index < tokens.size() && tokens[index].text == "@") {
    index += 2;
    if (index < tokens.size() && tokens[index].text == "(") {
      index = skip_balanced(tokens, index);
    }
  }
}

}  // namespace

ExtractionResult extract_graphql_sdl(const ExtractionContext& context) {
  ExtractionResult result;
  Emitter emit(context, result.fragment);
  auto tokens = tokenize(context.source, /*dotted_identifiers=*/false, /*hash_comments=*/true);
  std::erase_if(tokens, [](const Token& token) { return is_string_token(token.text); });  // descriptions

  // Root operation types: the defaults, then whatever a `schema { }` block says.
  std::unordered_map<std::string, std::string> roots{{"Query", "QUERY"}, {"Mutation", "MUTATION"}, {"Subscription", "SUBSCRIPTION"}};
  for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
    if (tokens[index].text == "schema" && (tokens[index + 1].text == "{" || tokens[index + 1].text == "@")) {
      roots.clear();
      std::size_t cursor = index + 1;
      skip_directives(tokens, cursor);
      if (cursor < tokens.size() && tokens[cursor].text == "{") {
        for (++cursor; cursor + 2 < tokens.size() && tokens[cursor].text != "}"; cursor += 3) {
          roots.emplace(tokens[cursor + 2].text, to_upper(tokens[cursor].text));
        }
      }
      break;
    }
  }

  std::unordered_map<std::string, std::string> schemas;  // type name -> id
  std::vector<PendingEdge> pending;
  const auto is_builtin = [](const std::string& name) {
    return std::ranges::find(kGraphqlBuiltins, name) != kGraphqlBuiltins.end();
  };

  std::size_t index = 0;
  while (index < tokens.size()) {
    const auto& token = tokens[index];
    const auto& text = token.text;
    if (text == "extend") {
      ++index;
      continue;
    }
    if (text == "schema") {
      std::size_t cursor = index + 1;
      skip_directives(tokens, cursor);
      index = cursor < tokens.size() && tokens[cursor].text == "{" ? skip_balanced(tokens, cursor) : cursor;
      continue;
    }
    if (text == "directive") {
      // `directive @name(args) repeatable on LOCATION | LOCATION`
      std::size_t cursor = index + 1;
      skip_directives(tokens, cursor);
      while (cursor < tokens.size() && tokens[cursor].text != "on") {
        ++cursor;
      }
      ++cursor;
      while (cursor < tokens.size() && (tokens[cursor].text == "|" || std::isupper(static_cast<unsigned char>(tokens[cursor].text.front())) != 0) &&
             cursor + 1 < tokens.size() && (tokens[cursor + 1].text == "|" || tokens[cursor].text == "|")) {
        ++cursor;
      }
      index = std::max(cursor + 1, index + 1);
      continue;
    }
    const bool is_definition = text == "type" || text == "interface" || text == "input" || text == "enum" ||
                               text == "union" || text == "scalar";
    if (!is_definition || index + 1 >= tokens.size()) {
      ++index;
      continue;
    }
    const auto kind = text;
    const auto name = tokens[index + 1].text;
    const auto id = emit.symbol(name, "schema", token.offset, {{"format", "graphql"}, {"graphql", kind}});
    schemas.emplace(name, id);
    std::size_t cursor = index + 2;
    if (cursor < tokens.size() && tokens[cursor].text == "implements") {
      ++cursor;
      if (cursor < tokens.size() && tokens[cursor].text == "&") {
        ++cursor;
      }
      while (cursor < tokens.size() && is_ident_char(tokens[cursor].text.front(), false)) {
        pending.push_back(PendingEdge{.source = id, .target_name = tokens[cursor].text, .relation = "implements"});
        ++cursor;
        if (cursor < tokens.size() && tokens[cursor].text == "&") {
          ++cursor;
        } else {
          break;
        }
      }
    }
    skip_directives(tokens, cursor);
    if (kind == "union") {
      // `= A | B | C`: members alternate with `|`; the next definition's keyword
      // is an identifier too, so the loop stops when no `|` precedes a name.
      if (cursor < tokens.size() && tokens[cursor].text == "=") {
        ++cursor;
        if (cursor < tokens.size() && tokens[cursor].text == "|") {
          ++cursor;  // a leading `|` is allowed
        }
        while (cursor < tokens.size() && is_ident_char(tokens[cursor].text.front(), false)) {
          pending.push_back(PendingEdge{.source = id, .target_name = tokens[cursor].text, .relation = "references"});
          ++cursor;
          if (cursor < tokens.size() && tokens[cursor].text == "|") {
            ++cursor;
          } else {
            break;
          }
        }
      }
      index = cursor;
      continue;
    }
    if (cursor >= tokens.size() || tokens[cursor].text != "{") {
      index = cursor;  // `scalar DateTime`, or a body-less extension
      continue;
    }
    const auto root = roots.find(name);
    const bool operations = kind == "type" && root != roots.end();
    ++cursor;
    while (cursor < tokens.size() && tokens[cursor].text != "}") {
      const auto& field_token = tokens[cursor];
      const auto field_name = field_token.text;
      if (!is_ident_char(field_name.front(), false)) {
        ++cursor;
        continue;
      }
      ++cursor;
      if (kind == "enum") {
        skip_directives(tokens, cursor);
        emit.field(id, name, field_name, field_token.offset, {});
        continue;
      }
      std::vector<std::string> argument_types;
      if (cursor < tokens.size() && tokens[cursor].text == "(") {
        ++cursor;
        while (cursor < tokens.size() && tokens[cursor].text != ")") {
          // `name: Type = default @directive`
          if (cursor + 1 < tokens.size() && tokens[cursor + 1].text == ":") {
            cursor += 2;
            argument_types.push_back(read_graphql_type(tokens, cursor).name);
            if (cursor < tokens.size() && tokens[cursor].text == "=") {
              ++cursor;
              if (cursor < tokens.size() && (tokens[cursor].text == "[" || tokens[cursor].text == "{")) {
                cursor = skip_balanced(tokens, cursor);
              } else {
                ++cursor;
              }
            }
            skip_directives(tokens, cursor);
            continue;
          }
          ++cursor;
        }
        ++cursor;  // ")"
      }
      if (cursor >= tokens.size() || tokens[cursor].text != ":") {
        continue;
      }
      ++cursor;
      const auto type = read_graphql_type(tokens, cursor);
      skip_directives(tokens, cursor);
      if (operations) {
        const auto endpoint = emit.endpoint(root->second, field_name, field_token.offset, {{"format", "graphql"}, {"protocol", "graphql"}});
        emit.edge(id, endpoint, "defines");
        pending.push_back(PendingEdge{.source = endpoint, .target_name = type.name, .relation = "RESPONDS_WITH"});
        for (const auto& argument : argument_types) {
          pending.push_back(PendingEdge{.source = endpoint, .target_name = argument, .relation = "ACCEPTS"});
        }
        continue;
      }
      emit.field(id, name, field_name, field_token.offset,
                 {{"type_text", type.text}, {"optional", type.non_null ? "false" : "true"}});
      pending.push_back(PendingEdge{.source = id, .target_name = type.name, .relation = "references"});
    }
    index = cursor + 1;
  }

  for (const auto& edge : pending) {
    if (edge.target_name.empty() || is_builtin(edge.target_name)) {
      continue;
    }
    if (const auto target = schemas.find(edge.target_name); target != schemas.end()) {
      emit.edge(edge.source, target->second, edge.relation);
    }
  }
  return result;
}

}  // namespace cgraph
