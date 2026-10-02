#include "cgraph/claim_contracts.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cgraph {
namespace {

constexpr std::string_view kProvides = "provides_contract";
constexpr std::string_view kUses = "uses_contract";
constexpr std::string_view kDecoder = "claim_decoder";
constexpr std::string_view kRead = "claim_read";

// --- tree-sitter helpers -----------------------------------------------------

[[nodiscard]] std::string_view type_of(TSNode node) { return ts_node_is_null(node) ? std::string_view{} : ts_node_type(node); }

[[nodiscard]] std::string text_of(TSNode node, std::string_view source) {
  if (ts_node_is_null(node)) {
    return {};
  }
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  if (end < start || end > source.size()) {
    return {};
  }
  return std::string(source.substr(start, end - start));
}

[[nodiscard]] TSNode field_of(TSNode node, const char* name) {
  return ts_node_child_by_field_name(node, name, static_cast<std::uint32_t>(std::strlen(name)));
}

// ts_node_named_child_count, 0 for a null node (a field the grammar lacks).
[[nodiscard]] std::uint32_t named_count(TSNode node) {
  return ts_node_is_null(node) ? 0 : ts_node_named_child_count(node);
}

[[nodiscard]] TSNode named_child(TSNode node, std::uint32_t index) {
  if (ts_node_is_null(node) || index >= named_count(node)) {
    return TSNode{};
  }
  return ts_node_named_child(node, index);
}

[[nodiscard]] TSNode first_named_of_type(TSNode node, std::string_view type) {
  if (ts_node_is_null(node)) {
    return TSNode{};
  }
  for (std::uint32_t index = 0; index < named_count(node); ++index) {
    const auto child = ts_node_named_child(node, index);
    if (type_of(child) == type) {
      return child;
    }
  }
  return TSNode{};
}

[[nodiscard]] std::uint32_t line_of(TSNode node) { return ts_node_start_point(node).row + 1; }

// Pre-order walk; `visit` returns false to skip the node's children.
void walk(TSNode node, const std::function<bool(TSNode)>& visit) {
  if (ts_node_is_null(node) || !visit(node)) {
    return;
  }
  for (std::uint32_t index = 0; index < named_count(node); ++index) {
    walk(ts_node_named_child(node, index), visit);
  }
}

[[nodiscard]] bool any_of(TSNode node, const std::function<bool(TSNode)>& predicate) {
  bool found = false;
  walk(node, [&](TSNode child) {
    if (found) {
      return false;
    }
    found = predicate(child);
    return !found;
  });
  return found;
}

// The value of a string literal with no interpolation, in any of the grammars
// (`"x"` in Kotlin, Java, TypeScript, Python, Go). nullopt for anything else,
// including an empty or interpolated string.
[[nodiscard]] std::optional<std::string> string_value(TSNode node, std::string_view source) {
  const auto type = type_of(node);
  if (type != "string_literal" && type != "string" && type != "interpreted_string_literal") {
    return std::nullopt;
  }
  std::optional<std::string> value;
  for (std::uint32_t index = 0; index < named_count(node); ++index) {
    const auto child = ts_node_named_child(node, index);
    const auto child_type = type_of(child);
    if (child_type == "string_start" || child_type == "string_end") {
      continue;
    }
    if (value || (child_type != "string_content" && child_type != "string_fragment" &&
                  child_type != "interpreted_string_literal_content")) {
      return std::nullopt;  // interpolation, escapes split the content, or a second part
    }
    value = text_of(child, source);
  }
  if (value && value->empty()) {
    return std::nullopt;
  }
  return value;
}

// The RFC 7519 registered claim a builder setter writes (`subject` -> `sub`),
// across jjwt (0.12 and the legacy `setX`), Nimbus and jose's SignJWT.
[[nodiscard]] std::string_view registered_claim_of_setter(std::string_view method) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 21> kSetters{{
      {"issuer", "iss"},       {"setIssuer", "iss"},         {"subject", "sub"},
      {"setSubject", "sub"},   {"audience", "aud"},          {"setAudience", "aud"},
      {"expiration", "exp"},   {"setExpiration", "exp"},     {"expirationTime", "exp"},
      {"setExpirationTime", "exp"}, {"notBefore", "nbf"},    {"setNotBefore", "nbf"},
      {"notBeforeTime", "nbf"}, {"issuedAt", "iat"},         {"setIssuedAt", "iat"},
      {"issueTime", "iat"},    {"id", "jti"},                {"setId", "jti"},
      {"jwtID", "jti"},        {"setJti", "jti"},            {"setJwtId", "jti"},
  }};
  for (const auto& [setter, claim] : kSetters) {
    if (setter == method) {
      return claim;
    }
  }
  return {};
}

[[nodiscard]] std::string unquote(std::string_view label) {
  if (label.size() >= 2 && (label.front() == '\'' || label.front() == '"') && label.back() == label.front()) {
    return std::string(label.substr(1, label.size() - 2));
  }
  return std::string(label);
}

// --- fact emission -----------------------------------------------------------

class Emitter {
 public:
  // Function spans and fields are indexed once per file; scope_at and
  // field_at run once per emitted fact.
  Emitter(const ExtractionContext& context, const Fragment& fragment, std::vector<RawRelation>& out)
      : context_(context), out_(out) {
    for (const auto& node : fragment.nodes) {
      if (node.kind == "file" && file_id_.empty()) {
        file_id_ = node.id;
      }
      if (!node.source_location) {
        continue;
      }
      if (node.kind == "function") {
        functions_.push_back({node.source_location->start_line, node.source_location->end_line, &node});
      } else if (node.kind == "field") {
        fields_.emplace(std::pair{node.source_location->start_line, unquote(node.label)}, node.id);
      }
    }
    std::ranges::sort(functions_, {}, &Span::start);
  }

  [[nodiscard]] std::string_view source() const { return context_.source; }

  // The innermost function node around `line`, else the file node.
  [[nodiscard]] std::string scope_at(std::uint32_t line) const {
    const Span* best = nullptr;
    for (const auto& span : functions_) {
      if (span.start > line) {
        break;  // sorted by start: no later span contains the line
      }
      if (span.end >= line && (best == nullptr || span.end - span.start < best->end - best->start)) {
        best = &span;
      }
    }
    return best != nullptr ? best->node->id : file_id_;
  }

  // The `field` node the member handler made for a declared member named
  // `name` (quotes ignored) on `line`; empty when there is none.
  [[nodiscard]] std::string field_at(std::uint32_t line, std::string_view name) const {
    const auto found = fields_.find(std::pair{line, std::string(name)});
    return found == fields_.end() ? std::string{} : found->second;
  }

  // The function node named `name` declared between `first_line` and `last_line`.
  [[nodiscard]] std::string function_named(std::string_view name, std::uint32_t first_line,
                                           std::uint32_t last_line) const {
    for (const auto& span : functions_) {
      if (span.node->label == name && span.start >= first_line && span.start <= last_line) {
        return span.node->id;
      }
    }
    return {};
  }

  void emit(std::string_view relation, const std::string& source_id, std::string_view target_label,
            const std::string& context) {
    if (source_id.empty() || context.empty()) {
      return;
    }
    if (!seen_.emplace(std::string(relation), source_id, std::string(target_label), context).second) {
      return;
    }
    out_.push_back(RawRelation{
        .source_id = source_id,
        .target_label = std::string(target_label),
        .relation = std::string(relation),
        .context = context,
        .source_file = context_.source_file,
        .allow_same_file = false,
    });
  }

  void claim(bool provides, const std::string& source_id, std::string_view name) {
    if (!name.empty()) {
      emit(provides ? kProvides : kUses, source_id, {}, "claim:" + std::string(name));
    }
  }

 private:
  struct Span {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    const Node* node = nullptr;
  };
  const ExtractionContext& context_;
  std::vector<RawRelation>& out_;
  std::string file_id_;
  std::vector<Span> functions_;
  std::map<std::pair<std::uint32_t, std::string>, std::string> fields_;
  std::set<std::tuple<std::string, std::string, std::string, std::string>> seen_;
};

// How far an expression is along a hand-written JWT payload decode:
// `token.split('.')`, its segment 1, base64-decoded, parsed as JSON.
enum class Payload : std::uint8_t { None, Split, Segment, Decoded, Parsed };

// --- Kotlin ------------------------------------------------------------------

struct KotlinCall {
  bool ok = false;
  TSNode receiver{};  // null for a bare call `claim(..)`
  std::string method;
  TSNode callee{};
  TSNode suffix{};  // call_suffix
};

[[nodiscard]] KotlinCall kotlin_call(TSNode call, std::string_view source) {
  KotlinCall result;
  if (type_of(call) != "call_expression") {
    return result;
  }
  result.callee = named_child(call, 0);
  result.suffix = first_named_of_type(call, "call_suffix");
  if (type_of(result.callee) == "navigation_expression") {
    result.receiver = named_child(result.callee, 0);
    const auto count = named_count(result.callee);
    const auto suffix = named_child(result.callee, count - 1);
    result.method = text_of(first_named_of_type(suffix, "simple_identifier"), source);
  } else if (type_of(result.callee) == "simple_identifier") {
    result.method = text_of(result.callee, source);
  }
  result.ok = !result.method.empty();
  return result;
}

// True when `node` is, or is a call chain rooted at, `Jwts.builder()` or
// `JWTClaimsSet.Builder()`.
[[nodiscard]] bool kotlin_builder_chain(TSNode node, std::string_view source) {
  while (!ts_node_is_null(node)) {
    const auto type = type_of(node);
    if (type == "call_expression") {
      const auto call = kotlin_call(node, source);
      if (!call.ok || ts_node_is_null(call.receiver)) {
        return false;
      }
      if (type_of(call.receiver) == "simple_identifier") {
        const auto root = text_of(call.receiver, source);
        return (root == "Jwts" && call.method == "builder") || (root == "JWTClaimsSet" && call.method == "Builder");
      }
      node = call.receiver;
    } else if (type == "navigation_expression") {
      node = named_child(node, 0);
    } else {
      return false;
    }
  }
  return false;
}

[[nodiscard]] TSNode kotlin_argument(TSNode suffix, std::uint32_t index) {
  const auto arguments = first_named_of_type(suffix, "value_arguments");
  const auto argument = named_child(arguments, index);
  if (type_of(argument) != "value_argument") {
    return TSNode{};
  }
  return named_child(argument, named_count(argument) - 1);
}

// Bare `claim("x", v)` calls inside a builder's `apply { }` lambda, not
// crossing into a nested lambda that rebinds `this`.
void kotlin_bare_claims(TSNode lambda, Emitter& emitter) {
  walk(lambda, [&](TSNode node) {
    if (type_of(node) != "call_expression") {
      return true;
    }
    const auto call = kotlin_call(node, emitter.source());
    if (!call.ok) {
      return true;
    }
    if (call.method == "apply" || call.method == "run" || call.method == "with") {
      return false;  // a new receiver
    }
    if (ts_node_is_null(call.receiver) && call.method == "claim") {
      if (const auto name = string_value(kotlin_argument(call.suffix, 0), emitter.source())) {
        emitter.claim(true, emitter.scope_at(line_of(node)), *name);
      }
    }
    return true;
  });
}

void kotlin_claims(TSNode root, Emitter& emitter) {
  const auto source = emitter.source();
  walk(root, [&](TSNode node) {
    const auto type = type_of(node);
    if (type == "call_expression") {
      const auto call = kotlin_call(node, source);
      if (call.ok && !ts_node_is_null(call.receiver) && kotlin_builder_chain(call.receiver, source)) {
        const auto scope = emitter.scope_at(line_of(node));
        if (call.method == "claim") {
          if (const auto name = string_value(kotlin_argument(call.suffix, 0), source)) {
            emitter.claim(true, scope, *name);
          }
        } else if (call.method == "apply" || call.method == "run") {
          kotlin_bare_claims(first_named_of_type(call.suffix, "annotated_lambda"), emitter);
        } else if (const auto registered = registered_claim_of_setter(call.method); !registered.empty()) {
          emitter.claim(true, scope, registered);
        }
      }
      return true;
    }
    if (type != "function_declaration") {
      return true;
    }
    // A hand-written payload decode, followed along its data path in this
    // function: `token.split('.')`, its segment 1 (`.getOrNull(1)`, `[1]`), a
    // `Base64...decode(segment)`, `Json.parseToJsonElement(decoded..)`, then
    // `.jsonObject["x"]` on that parsed payload.
    std::unordered_map<std::string, Payload> variables;
    const std::function<Payload(TSNode)> level = [&](TSNode expression) -> Payload {
      const auto expression_type = type_of(expression);
      if (expression_type == "parenthesized_expression" || expression_type == "elvis_expression") {
        return level(named_child(expression, 0));
      }
      if (expression_type == "simple_identifier") {
        const auto found = variables.find(text_of(expression, source));
        return found == variables.end() ? Payload::None : found->second;
      }
      if (expression_type == "indexing_expression") {
        return level(named_child(expression, 0)) == Payload::Split &&
                       text_of(named_child(first_named_of_type(expression, "indexing_suffix"), 0), source) == "1"
                   ? Payload::Segment
                   : Payload::None;
      }
      if (expression_type == "navigation_expression") {
        const auto suffix = named_child(expression, named_count(expression) - 1);
        return text_of(first_named_of_type(suffix, "simple_identifier"), source) == "jsonObject" &&
                       level(named_child(expression, 0)) == Payload::Parsed
                   ? Payload::Parsed
                   : Payload::None;
      }
      const auto call = kotlin_call(expression, source);
      if (!call.ok) {
        return Payload::None;
      }
      const auto argument = kotlin_argument(call.suffix, 0);
      const auto receiver = ts_node_is_null(call.receiver) ? Payload::None : level(call.receiver);
      if (call.method == "split") {
        return text_of(argument, source) == "'.'" || string_value(argument, source) == "." ? Payload::Split
                                                                                          : Payload::None;
      }
      if (call.method == "getOrNull" || call.method == "get" || call.method == "elementAt") {
        return receiver == Payload::Split && text_of(argument, source) == "1" ? Payload::Segment : Payload::None;
      }
      if (call.method == "replace" || call.method == "padEnd" || call.method == "trim") {
        return receiver == Payload::Segment ? Payload::Segment : Payload::None;
      }
      if (call.method == "decode" && text_of(call.callee, source).starts_with("Base64")) {
        return level(argument) >= Payload::Segment ? Payload::Decoded : Payload::None;
      }
      if (call.method == "decodeToString" || call.method == "toString") {
        return receiver == Payload::Decoded ? Payload::Decoded : Payload::None;
      }
      if (call.method == "String" && ts_node_is_null(call.receiver)) {
        return level(argument) == Payload::Decoded ? Payload::Decoded : Payload::None;
      }
      if (call.method == "parseToJsonElement") {
        return level(argument) >= Payload::Decoded ? Payload::Parsed : Payload::None;
      }
      return Payload::None;
    };
    walk(node, [&](TSNode child) {
      const auto child_type = type_of(child);
      if (child_type == "property_declaration") {
        const auto name = text_of(first_named_of_type(first_named_of_type(child, "variable_declaration"),
                                                      "simple_identifier"),
                                  source);
        const auto value = named_child(child, named_count(child) - 1);
        if (!name.empty() && type_of(value) != "variable_declaration") {
          variables[name] = level(value);
        }
      } else if (child_type == "assignment") {
        // `data = load(data)`: the variable holds what was last stored in it.
        const auto target = named_child(first_named_of_type(child, "directly_assignable_expression"), 0);
        const auto value = named_child(child, named_count(child) - 1);
        if (type_of(target) == "simple_identifier" && type_of(value) != "null_literal") {
          variables[text_of(target, source)] = level(value);
        }
      } else if (child_type == "indexing_expression" && level(named_child(child, 0)) == Payload::Parsed) {
        if (const auto key = string_value(named_child(first_named_of_type(child, "indexing_suffix"), 0), source)) {
          emitter.claim(false, emitter.scope_at(line_of(child)), *key);
        }
      }
      return true;
    });
    return true;
  });
}

// --- Java --------------------------------------------------------------------

[[nodiscard]] bool java_builder_chain(TSNode node, std::string_view source) {
  while (!ts_node_is_null(node)) {
    const auto type = type_of(node);
    if (type == "method_invocation") {
      const auto object = field_of(node, "object");
      if (type_of(object) == "identifier") {
        return text_of(object, source) == "Jwts" && text_of(field_of(node, "name"), source) == "builder";
      }
      node = object;
    } else if (type == "object_creation_expression") {
      return text_of(field_of(node, "type"), source) == "JWTClaimsSet.Builder";
    } else {
      return false;
    }
  }
  return false;
}

void java_claims(TSNode root, Emitter& emitter) {
  const auto source = emitter.source();
  walk(root, [&](TSNode node) {
    if (type_of(node) != "method_invocation" || !java_builder_chain(field_of(node, "object"), source)) {
      return true;
    }
    const auto method = text_of(field_of(node, "name"), source);
    const auto scope = emitter.scope_at(line_of(node));
    if (method == "claim") {
      if (const auto name = string_value(named_child(field_of(node, "arguments"), 0), source)) {
        emitter.claim(true, scope, *name);
      }
    } else if (const auto registered = registered_claim_of_setter(method); !registered.empty()) {
      emitter.claim(true, scope, registered);
    }
    return true;
  });
}

// --- Go ----------------------------------------------------------------------

// The JSON name in a struct tag (`json:"session_id,omitempty"` -> session_id);
// empty for no json key, `-`, or an empty name.
[[nodiscard]] std::string go_json_name(std::string_view tag) {
  const auto at = tag.find("json:\"");
  if (at == std::string_view::npos) {
    return {};
  }
  const auto start = at + 6;
  const auto end = tag.find('"', start);
  if (end == std::string_view::npos) {
    return {};
  }
  auto name = tag.substr(start, end - start);
  name = name.substr(0, name.find(','));
  return name == "-" ? std::string{} : std::string(name);
}

[[nodiscard]] std::string go_bare_type(std::string text) {
  while (!text.empty() && (text.front() == '*' || text.front() == '&')) {
    text.erase(0, 1);
  }
  return text;
}

void go_claims(TSNode root, Emitter& emitter) {
  const auto source = emitter.source();
  // golang-jwt (and its predecessors) under the name this file imports it as.
  std::unordered_set<std::string> jwt_packages;
  // Struct name -> its declaration, and field name -> field type per struct.
  std::unordered_map<std::string, TSNode> structs;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> field_types;
  walk(root, [&](TSNode node) {
    const auto type = type_of(node);
    if (type == "import_spec") {
      const auto path = text_of(field_of(node, "path"), source);
      if (path.find("golang-jwt/jwt") != std::string::npos || path.find("dgrijalva/jwt-go") != std::string::npos ||
          path.find("form3tech-oss/jwt-go") != std::string::npos) {
        const auto alias = text_of(field_of(node, "name"), source);
        jwt_packages.insert(alias.empty() ? std::string("jwt") : alias);
      }
      return false;
    }
    if (type == "type_spec" && type_of(field_of(node, "type")) == "struct_type") {
      const auto name = text_of(field_of(node, "name"), source);
      structs[name] = field_of(node, "type");
      walk(field_of(node, "type"), [&](TSNode field) {
        if (type_of(field) != "field_declaration") {
          return true;
        }
        const auto field_type = go_bare_type(text_of(field_of(field, "type"), source));
        for (std::uint32_t index = 0; index < ts_node_child_count(field); ++index) {
          const char* field_name = ts_node_field_name_for_child(field, index);
          if (field_name != nullptr && std::string_view(field_name) == "name") {
            field_types[name][text_of(ts_node_child(field, index), source)] = field_type;
          }
        }
        return false;
      });
    }
    return true;
  });
  if (structs.empty()) {
    return;
  }

  std::unordered_set<std::string> claims_types;
  // (i) embeds the library's claims.
  for (const auto& [name, body] : structs) {
    walk(body, [&](TSNode field) {
      if (type_of(field) != "field_declaration") {
        return true;
      }
      // `jwt.RegisteredClaims` and `*jwt.RegisteredClaims` alike: the grammar
      // keeps an embedded field's `*` as a bare token beside its qualified_type.
      const auto embedded = field_of(field, "type");
      if (ts_node_is_null(field_of(field, "name")) && type_of(embedded) == "qualified_type" &&
          jwt_packages.contains(text_of(field_of(embedded, "package"), source))) {
        const auto library_type = text_of(field_of(embedded, "name"), source);
        if (library_type == "RegisteredClaims" || library_type == "StandardClaims") {
          claims_types.insert(name);
        }
      }
      return false;
    });
  }
  const auto selector_call = [&](TSNode call, std::string_view operand, std::string_view field) {
    const auto function = field_of(call, "function");
    return type_of(call) == "call_expression" && type_of(function) == "selector_expression" &&
           text_of(field_of(function, "operand"), source) == operand && text_of(field_of(function, "field"), source) == field;
  };
  // `base64.RawURLEncoding.DecodeString(seg)`, often in a helper beside the decoder.
  const bool file_base64 = any_of(root, [&](TSNode node) {
    const auto function = field_of(node, "function");
    return type_of(node) == "call_expression" && type_of(function) == "selector_expression" &&
           text_of(field_of(function, "field"), source) == "DecodeString" &&
           text_of(field_of(function, "operand"), source).starts_with("base64.");
  });
  walk(root, [&](TSNode node) {
    const auto type = type_of(node);
    // (ii) the claims argument of jwt.ParseWithClaims(token, &S{}, keyFunc).
    if (type == "call_expression") {
      const auto function = field_of(node, "function");
      if (type_of(function) == "selector_expression" &&
          jwt_packages.contains(text_of(field_of(function, "operand"), source)) &&
          text_of(field_of(function, "field"), source) == "ParseWithClaims") {
        const auto claims = named_child(field_of(node, "arguments"), 1);
        const auto literal = type_of(claims) == "unary_expression" ? field_of(claims, "operand") : claims;
        if (type_of(literal) == "composite_literal") {
          claims_types.insert(go_bare_type(text_of(field_of(literal, "type"), source)));
        } else if (type_of(literal) == "identifier") {
          // `claims := &Claims{}` (or `var claims Claims`) earlier in the same function.
          auto function = ts_node_parent(node);
          while (!ts_node_is_null(function) && type_of(function) != "function_declaration" &&
                 type_of(function) != "method_declaration" && type_of(function) != "func_literal") {
            function = ts_node_parent(function);
          }
          const auto variable = text_of(literal, source);
          walk(function, [&](TSNode child) {
            if (type_of(child) == "short_var_declaration") {
              const auto left = field_of(child, "left");
              const auto right = field_of(child, "right");
              for (std::uint32_t index = 0; index < named_count(left); ++index) {
                auto value = named_child(right, index);
                if (type_of(value) == "unary_expression") {
                  value = field_of(value, "operand");
                }
                if (text_of(named_child(left, index), source) == variable && type_of(value) == "composite_literal") {
                  claims_types.insert(go_bare_type(text_of(field_of(value, "type"), source)));
                }
              }
            } else if (type_of(child) == "var_spec" && text_of(field_of(child, "name"), source) == variable) {
              claims_types.insert(go_bare_type(text_of(field_of(child, "type"), source)));
            }
            return true;
          });
        }
      }
      return true;
    }
    if (type != "function_declaration" && type != "method_declaration") {
      return true;
    }
    // (iii) a payload decode by hand: `parts := strings.Split(token, ".")`,
    // `b, err := decode(parts[1])` (the payload segment, never the header's
    // parts[0]), then `json.Unmarshal(b, &target)` into the struct, in this
    // function, in a file that base64-decodes.
    std::unordered_set<std::string> split_vars;
    std::unordered_set<std::string> payload_vars;
    std::vector<TSNode> targets;
    std::unordered_map<std::string, std::string> local_types;
    walk(node, [&](TSNode child) {
      const auto child_type = type_of(child);
      if (child_type == "var_spec") {
        const auto var_type = go_bare_type(text_of(field_of(child, "type"), source));
        for (std::uint32_t index = 0; index < ts_node_child_count(child); ++index) {
          const char* field_name = ts_node_field_name_for_child(child, index);
          if (field_name != nullptr && std::string_view(field_name) == "name" && !var_type.empty()) {
            local_types[text_of(ts_node_child(child, index), source)] = var_type;
          }
        }
      } else if (child_type == "short_var_declaration" || child_type == "assignment_statement") {
        const auto left = field_of(child, "left");
        const auto right = field_of(child, "right");
        const auto first = text_of(named_child(left, 0), source);
        if (named_count(right) == 1 && selector_call(named_child(right, 0), "strings", "Split") &&
            string_value(named_child(field_of(named_child(right, 0), "arguments"), 1), source) == ".") {
          split_vars.insert(first);
        } else if (any_of(right, [&](TSNode part) {
                     return type_of(part) == "index_expression" &&
                            split_vars.contains(text_of(field_of(part, "operand"), source)) &&
                            text_of(field_of(part, "index"), source) == "1";
                   })) {
          payload_vars.insert(first);
        }
        for (std::uint32_t index = 0; index < named_count(left); ++index) {
          auto value = named_child(right, index);
          if (type_of(value) == "unary_expression") {
            value = field_of(value, "operand");
          }
          if (type_of(value) == "composite_literal") {
            local_types[text_of(named_child(left, index), source)] = go_bare_type(text_of(field_of(value, "type"), source));
          }
        }
      } else if (child_type == "call_expression" && selector_call(child, "json", "Unmarshal")) {
        const auto arguments = field_of(child, "arguments");
        const auto target = named_child(arguments, 1);
        if (payload_vars.contains(text_of(named_child(arguments, 0), source)) && type_of(target) == "unary_expression") {
          targets.push_back(field_of(target, "operand"));
        }
      }
      return true;
    });
    if (targets.empty() || !file_base64) {
      return true;
    }
    for (const auto target : targets) {
      std::string target_type;
      if (type_of(target) == "identifier") {
        const auto local = local_types.find(text_of(target, source));
        target_type = local == local_types.end() ? std::string{} : local->second;
      } else if (type_of(target) == "selector_expression" && type_of(field_of(target, "operand")) == "identifier") {
        const auto owner = local_types.find(text_of(field_of(target, "operand"), source));
        if (owner != local_types.end()) {
          const auto fields = field_types.find(owner->second);
          if (fields != field_types.end()) {
            const auto field = fields->second.find(text_of(field_of(target, "field"), source));
            target_type = field == fields->second.end() ? std::string{} : field->second;
          }
        }
      }
      if (structs.contains(target_type)) {
        claims_types.insert(target_type);
      }
    }
    return true;
  });

  for (const auto& name : claims_types) {
    const auto body = structs.find(name);
    if (body == structs.end()) {
      continue;
    }
    walk(body->second, [&](TSNode field) {
      if (type_of(field) != "field_declaration") {
        return true;
      }
      const auto claim = go_json_name(text_of(field_of(field, "tag"), source));
      if (claim.empty()) {
        return false;
      }
      for (std::uint32_t index = 0; index < ts_node_child_count(field); ++index) {
        const char* field_name = ts_node_field_name_for_child(field, index);
        if (field_name != nullptr && std::string_view(field_name) == "name") {
          const auto member = ts_node_child(field, index);
          emitter.claim(false, emitter.field_at(line_of(member), text_of(member, source)), claim);
        }
      }
      return false;
    });
  }
}

// --- TypeScript / JavaScript -------------------------------------------------

// What one pass over a JS/TS file learns: the JWT libraries it imports, the
// names it imports from its own code (a relative or aliased module), and its
// interfaces and object type aliases.
struct JsFile {
  std::unordered_set<std::string> jsonwebtoken;            // module objects: `jwt` in jwt.sign
  std::unordered_map<std::string, std::string> functions;  // local name -> library function
  std::unordered_set<std::string> local_imports;           // imported from `./x`, `@/x`, `~/x`, `#x`
  std::unordered_map<std::string, TSNode> types;           // same-file interface / object type alias
};

// A module specifier that names the project's own code rather than a package.
[[nodiscard]] bool js_local_module(std::string_view module) {
  return module.starts_with('.') || module.starts_with('/') || module.starts_with("@/") || module.starts_with('~') ||
         module.starts_with('#');
}

[[nodiscard]] JsFile js_scan(TSNode root, std::string_view source) {
  JsFile file;
  walk(root, [&](TSNode node) {
    const auto type = type_of(node);
    if (type == "interface_declaration" ||
        (type == "type_alias_declaration" && type_of(field_of(node, "value")) == "object_type")) {
      file.types.emplace(text_of(field_of(node, "name"), source), node);
      return false;
    }
    if (type == "import_statement") {
      const auto module = string_value(field_of(node, "source"), source).value_or("");
      const bool local = js_local_module(module);
      walk(first_named_of_type(node, "import_clause"), [&](TSNode part) {
        const auto part_type = type_of(part);
        if (part_type == "import_clause") {
          const auto default_import = first_named_of_type(part, "identifier");
          if (!ts_node_is_null(default_import)) {
            const auto name = text_of(default_import, source);
            if (local) {
              file.local_imports.insert(name);
            } else if (module == "jsonwebtoken") {
              file.jsonwebtoken.insert(name);
            } else if (module == "jwt-decode") {
              file.functions[name] = "jwtDecode";
            }
          }
          return true;
        }
        if (part_type == "namespace_import") {
          if (module == "jsonwebtoken") {
            file.jsonwebtoken.insert(text_of(first_named_of_type(part, "identifier"), source));
          }
          return false;
        }
        if (part_type == "import_specifier") {
          const auto name = text_of(field_of(part, "name"), source);
          const auto alias = text_of(field_of(part, "alias"), source);
          const auto local_name = alias.empty() ? name : alias;
          if (local) {
            file.local_imports.insert(local_name);
          } else if ((module == "jsonwebtoken" && (name == "sign" || name == "verify" || name == "decode")) ||
                     (module == "jose" && (name == "SignJWT" || name == "decodeJwt" || name == "jwtVerify")) ||
                     (module == "jwt-decode" && name == "jwtDecode")) {
            file.functions[local_name] = module == "jsonwebtoken" ? "jsonwebtoken." + name : name;
          }
          return false;
        }
        return true;
      });
      return false;
    }
    if (type == "variable_declarator") {
      // const jwt = require('jsonwebtoken')
      const auto value = field_of(node, "value");
      if (type_of(value) == "call_expression" && text_of(field_of(value, "function"), source) == "require" &&
          string_value(named_child(field_of(value, "arguments"), 0), source) == "jsonwebtoken" &&
          type_of(field_of(node, "name")) == "identifier") {
        file.jsonwebtoken.insert(text_of(field_of(node, "name"), source));
      }
    }
    return true;
  });
  return file;
}

[[nodiscard]] TSNode js_unwrap(TSNode node) {
  while (type_of(node) == "await_expression" || type_of(node) == "parenthesized_expression" ||
         type_of(node) == "non_null_expression") {
    node = named_child(node, 0);
  }
  return node;
}

// The library function a call goes to (`jwtDecode`, `jsonwebtoken.verify`, ...), empty otherwise.
[[nodiscard]] std::string js_library_call(TSNode call, const JsFile& file, std::string_view source) {
  if (type_of(call) != "call_expression" && type_of(call) != "new_expression") {
    return {};
  }
  auto function = type_of(call) == "new_expression" ? field_of(call, "constructor") : field_of(call, "function");
  if (type_of(function) == "await_expression") {
    function = named_child(function, 0);  // `await jwtVerify<T>(..)` parses as a call of `await jwtVerify`
  }
  if (type_of(function) == "identifier") {
    const auto found = file.functions.find(text_of(function, source));
    return found == file.functions.end() ? std::string{} : found->second;
  }
  if (type_of(function) == "member_expression" && file.jsonwebtoken.contains(text_of(field_of(function, "object"), source))) {
    return "jsonwebtoken." + text_of(field_of(function, "property"), source);
  }
  return {};
}

[[nodiscard]] bool js_decodes(std::string_view library) {
  return library == "jwtDecode" || library == "decodeJwt" || library == "jwtVerify" ||
         library == "jsonwebtoken.verify" || library == "jsonwebtoken.decode";
}

// True for a library call whose result wraps the payload: jose's
// `jwtVerify(..)` (`{ payload, protectedHeader }`) and jsonwebtoken's `verify` /
// `decode` with `complete: true` (`{ header, payload, signature }`).
[[nodiscard]] bool js_wraps(TSNode call, const JsFile& file, std::string_view source) {
  call = js_unwrap(call);
  const auto library = js_library_call(call, file, source);
  if (library == "jwtVerify") {
    return true;
  }
  if (library != "jsonwebtoken.verify" && library != "jsonwebtoken.decode") {
    return false;
  }
  return any_of(field_of(call, "arguments"), [&](TSNode node) {
    return type_of(node) == "pair" && text_of(field_of(node, "key"), source) == "complete" &&
           text_of(field_of(node, "value"), source) == "true";
  });
}

[[nodiscard]] bool js_function_like(std::string_view type) {
  return type == "function_declaration" || type == "function_expression" || type == "arrow_function" ||
         type == "method_definition" || type == "function" || type == "generator_function_declaration";
}

// The keys of an object literal (`{ roles, 'tenant_id': t }`).
void js_object_keys(TSNode object, const std::string& scope, Emitter& emitter) {
  if (type_of(object) != "object") {
    return;
  }
  for (std::uint32_t index = 0; index < named_count(object); ++index) {
    const auto member = ts_node_named_child(object, index);
    if (type_of(member) == "shorthand_property_identifier") {
      emitter.claim(true, scope, text_of(member, emitter.source()));
    } else if (type_of(member) == "pair") {
      const auto key = field_of(member, "key");
      if (type_of(key) == "property_identifier") {
        emitter.claim(true, scope, text_of(key, emitter.source()));
      } else if (const auto name = string_value(key, emitter.source())) {
        emitter.claim(true, scope, *name);
      }
    }
  }
}

// The one same-file type a type names: `T`, or `T` inside `Promise<T>` and
// `T | null | undefined`. Empty for anything else (`Result<T, E>`, `A | B`).
[[nodiscard]] std::string js_single_type(TSNode node, const JsFile& file, std::string_view source) {
  while (type_of(node) == "type_annotation" || type_of(node) == "parenthesized_type") {
    node = named_child(node, 0);
  }
  const auto type = type_of(node);
  if (type == "type_identifier") {
    const auto name = text_of(node, source);
    return file.types.contains(name) ? name : std::string{};
  }
  if (type == "generic_type" && text_of(field_of(node, "name"), source) == "Promise") {
    const auto arguments = field_of(node, "type_arguments");
    return named_count(arguments) == 1 ? js_single_type(named_child(arguments, 0), file, source)
                                                     : std::string{};
  }
  if (type == "union_type") {
    std::string single;
    for (std::uint32_t index = 0; index < named_count(node); ++index) {
      const auto member = ts_node_named_child(node, index);
      const auto text = text_of(member, source);
      if (text == "null" || text == "undefined") {
        continue;
      }
      const auto name = js_single_type(member, file, source);
      if (name.empty() || !single.empty()) {
        return {};
      }
      single = name;
    }
    return single;
  }
  return {};
}

// The declared properties of an interface or object type alias.
[[nodiscard]] std::vector<std::pair<TSNode, std::string>> js_properties(TSNode declaration, std::string_view source) {
  std::vector<std::pair<TSNode, std::string>> properties;
  const auto body = type_of(declaration) == "interface_declaration" ? field_of(declaration, "body")
                                                                     : field_of(declaration, "value");
  for (std::uint32_t index = 0; index < named_count(body); ++index) {
    const auto member = ts_node_named_child(body, index);
    if (type_of(member) != "property_signature") {
      continue;
    }
    const auto key = field_of(member, "name");
    if (type_of(key) == "property_identifier") {
      properties.emplace_back(member, text_of(key, source));
    } else if (const auto name = string_value(key, source)) {
      properties.emplace_back(member, *name);
    }
  }
  return properties;
}

// Follows a function body's data path towards a decoded JWT payload. A
// library decode (`jwtDecode(t)`, `jwt.verify(..)`, jose's `{ payload }`) is
// the payload; by hand it is `t.split('.')`, its `[1]`, a base64 decode of
// that (`Buffer.from(x, 'base64')`, `atob(x)`, a helper named `*base64*`),
// then `JSON.parse` of the decoded string.
class JsPayloadFlow {
 public:
  JsPayloadFlow(const JsFile& file, std::string_view source) : file_(file), source_(source) {}

  [[nodiscard]] Payload level(TSNode expression) const {
    expression = js_unwrap(expression);
    const auto type = type_of(expression);
    if (type == "as_expression" || type == "satisfies_expression") {
      return level(named_child(expression, 0));
    }
    if (type == "identifier") {
      const auto found = variables_.find(text_of(expression, source_));
      return found == variables_.end() ? Payload::None : found->second;
    }
    if (type == "subscript_expression") {
      return level(field_of(expression, "object")) == Payload::Split &&
                     text_of(field_of(expression, "index"), source_) == "1"
                 ? Payload::Segment
                 : Payload::None;
    }
    if (type == "binary_expression" && text_of(field_of(expression, "operator"), source_) == "+") {
      return level(field_of(expression, "left")) == Payload::Segment ||
                     level(field_of(expression, "right")) == Payload::Segment
                 ? Payload::Segment
                 : Payload::None;
    }
    // `result.payload` of jose's `{ payload, protectedHeader }` or of
    // jsonwebtoken's `{ header, payload, signature }` (`complete: true`).
    if (type == "member_expression" && text_of(field_of(expression, "property"), source_) == "payload") {
      const auto object = js_unwrap(field_of(expression, "object"));
      return (type_of(object) == "identifier" && wrappers_.contains(text_of(object, source_))) || wraps(object)
                 ? Payload::Parsed
                 : Payload::None;
    }
    if (type != "call_expression") {
      return Payload::None;
    }
    if (js_decodes(js_library_call(expression, file_, source_))) {
      return wraps(expression) ? Payload::None : Payload::Parsed;  // a wrapper is not the payload itself
    }
    const auto function = field_of(expression, "function");
    const auto arguments = field_of(expression, "arguments");
    const auto first = named_child(arguments, 0);
    const auto function_text = text_of(function, source_);
    if (function_text == "JSON.parse") {
      return level(first) >= Payload::Decoded ? Payload::Parsed : Payload::None;
    }
    if (function_text == "Buffer.from") {
      const auto encoding = string_value(named_child(arguments, 1), source_);
      return (encoding == "base64" || encoding == "base64url") && level(first) >= Payload::Segment ? Payload::Decoded
                                                                                                 : Payload::None;
    }
    if (type_of(function) == "identifier") {
      std::string lowered = function_text;
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      return (function_text == "atob" || lowered.find("base64") != std::string::npos) && level(first) >= Payload::Segment
                 ? Payload::Decoded
                 : Payload::None;
    }
    if (type_of(function) != "member_expression") {
      return Payload::None;
    }
    const auto method = text_of(field_of(function, "property"), source_);
    const auto receiver = level(field_of(function, "object"));
    if (method == "split") {
      return string_value(first, source_) == "." ? Payload::Split : Payload::None;
    }
    if (method == "replace" || method == "replaceAll" || method == "padEnd" || method == "trim") {
      return receiver == Payload::Segment ? Payload::Segment : Payload::None;
    }
    if (method == "toString") {
      return receiver == Payload::Decoded ? Payload::Decoded : Payload::None;
    }
    return Payload::None;
  }

  [[nodiscard]] bool wraps(TSNode call) const { return js_wraps(call, file_, source_); }

  // Records a declaration or assignment, in source order. The variable holds
  // what was last stored in it: a later non-payload value clears it (a bare
  // `null` / `undefined` placeholder leaves it as it was).
  void bind(TSNode pattern, TSNode value) {
    const auto value_level = level(value);
    if (type_of(pattern) == "identifier") {
      const auto name = text_of(pattern, source_);
      const auto stored = js_unwrap(value);
      if (type_of(stored) == "null" || type_of(stored) == "undefined") {
        return;
      }
      variables_[name] = value_level;
      if (wraps(stored)) {
        wrappers_.insert(name);
      } else {
        wrappers_.erase(name);
      }
    } else if (type_of(pattern) == "object_pattern" && wraps(value)) {
      // jose: `const { payload } = await jwtVerify(token, key)`
      for (std::uint32_t index = 0; index < named_count(pattern); ++index) {
        const auto member = ts_node_named_child(pattern, index);
        if (type_of(member) == "shorthand_property_identifier_pattern" && text_of(member, source_) == "payload") {
          variables_["payload"] = Payload::Parsed;
        } else if (type_of(member) == "pair_pattern" && text_of(field_of(member, "key"), source_) == "payload" &&
                   type_of(field_of(member, "value")) == "identifier") {
          variables_[text_of(field_of(member, "value"), source_)] = Payload::Parsed;
        }
      }
    } else if (type_of(pattern) == "array_pattern" && value_level == Payload::Split) {
      // const [header, payload, signature] = token.split('.'), or `[, payload]`:
      // the element after the first comma.
      std::uint32_t commas = 0;
      for (std::uint32_t index = 0; index < ts_node_child_count(pattern); ++index) {
        const auto element = ts_node_child(pattern, index);
        if (type_of(element) == ",") {
          ++commas;
        } else if (commas == 1 && type_of(element) == "identifier") {
          variables_[text_of(element, source_)] = Payload::Segment;
        }
      }
    }
  }

 private:
  const JsFile& file_;
  std::string_view source_;
  std::unordered_map<std::string, Payload> variables_;
  std::unordered_set<std::string> wrappers_;  // variables holding a library wrapper (`r` in `r.payload`)
};

// The claims type a decoder function returns, from its own data path: a
// returned `x as T` where x is the decoded payload names T; a returned payload
// with no cast names the declared return type when that is one same-file type.
// Empty when nothing returned is the payload.
[[nodiscard]] std::set<std::string> js_decoder_types(TSNode function, const JsFile& file, std::string_view source) {
  JsPayloadFlow flow(file, source);
  std::set<std::string> types;
  const auto returned = [&](TSNode value) {
    value = js_unwrap(value);
    if (type_of(value) == "as_expression") {
      if (flow.level(named_child(value, 0)) == Payload::Parsed) {
        if (const auto name = js_single_type(named_child(value, 1), file, source); !name.empty()) {
          types.insert(name);
        }
      }
      return;
    }
    if (flow.level(value) == Payload::Parsed) {
      if (const auto name = js_single_type(field_of(function, "return_type"), file, source); !name.empty()) {
        types.insert(name);
      }
    }
  };
  const auto body = field_of(function, "body");
  if (type_of(body) != "statement_block") {
    returned(body);  // `(t: string): T => jwtDecode(t)`
    return types;
  }
  walk(body, [&](TSNode node) {
    const auto type = type_of(node);
    if (js_function_like(type)) {
      return false;  // a nested function's returns are its own
    }
    if (type == "variable_declarator") {
      flow.bind(field_of(node, "name"), field_of(node, "value"));
    } else if (type == "assignment_expression") {
      flow.bind(field_of(node, "left"), field_of(node, "right"));
    } else if (type == "return_statement") {
      returned(named_child(node, 0));
    }
    return true;
  });
  return types;
}

// Every identifier a parameter list or binding pattern declares.
void js_declared_names(TSNode pattern, std::string_view source, std::vector<std::string>& names) {
  walk(pattern, [&](TSNode node) {
    const auto type = type_of(node);
    if (type == "identifier" || type == "shorthand_property_identifier_pattern") {
      names.push_back(text_of(node, source));
      return false;
    }
    if (type == "type_annotation" || type == "default_value") {
      return false;  // `x: T = init` declares only x
    }
    if (type == "pair_pattern") {
      js_declared_names(field_of(node, "value"), source, names);
      return false;
    }
    return true;
  });
}

void js_claims(TSNode root, Emitter& emitter) {
  const auto source = emitter.source();
  const auto file = js_scan(root, source);
  // Providers and decoders need a JWT library or a hand-written decode.
  const bool may_decode = source.find("jsonwebtoken") != std::string_view::npos ||
                          source.find("jose") != std::string_view::npos ||
                          source.find("jwt-decode") != std::string_view::npos ||
                          source.find("JSON.parse") != std::string_view::npos;

  std::set<std::string> claims_types;
  // Decoder function name -> (node id, the claims types it returns).
  std::map<std::string, std::pair<std::string, std::set<std::string>>> decoders;
  if (may_decode) {
    walk(root, [&](TSNode node) {
      const auto type = type_of(node);
      if (type == "call_expression" || type == "new_expression") {
        const auto library = js_library_call(node, file, source);
        if (library == "jsonwebtoken.sign" || library == "SignJWT") {
          js_object_keys(named_child(field_of(node, "arguments"), 0), emitter.scope_at(line_of(node)), emitter);
        }
        // `jwtDecode<T>(..)`, `decodeJwt<T>(..)`, `jwtVerify<T>(..)` type the payload as T.
        if (js_decodes(library)) {
          const auto arguments = field_of(node, "type_arguments");
          if (named_count(arguments) == 1) {
            if (const auto name = js_single_type(named_child(arguments, 0), file, source); !name.empty()) {
              claims_types.insert(name);
            }
          }
        }
        // `new SignJWT({..}).setSubject(..)`: the registered setters on its chain.
        if (type == "call_expression" && type_of(field_of(node, "function")) == "member_expression") {
          const auto member = field_of(node, "function");
          const auto registered = registered_claim_of_setter(text_of(field_of(member, "property"), source));
          auto chain = field_of(member, "object");
          while (!registered.empty() && type_of(chain) == "call_expression" &&
                 type_of(field_of(chain, "function")) == "member_expression") {
            chain = field_of(field_of(chain, "function"), "object");
          }
          if (!registered.empty() && js_library_call(chain, file, source) == "SignJWT") {
            emitter.claim(true, emitter.scope_at(line_of(node)), registered);
          }
        }
        return true;
      }
      // `jwt.verify(token, key) as T`: the cast types the library's payload.
      if (type == "as_expression" && js_decodes(js_library_call(js_unwrap(named_child(node, 0)), file, source)) &&
          !js_wraps(named_child(node, 0), file, source)) {
        if (const auto name = js_single_type(named_child(node, 1), file, source); !name.empty()) {
          claims_types.insert(name);
        }
        return true;
      }
      // A function that returns the decoded payload.
      TSNode function{};
      std::string name;
      std::uint32_t first_line = 0;
      if (type == "function_declaration") {
        function = node;
        name = text_of(field_of(node, "name"), source);
        first_line = line_of(node);
      } else if (type == "variable_declarator" && type_of(field_of(node, "name")) == "identifier" &&
                 (type_of(field_of(node, "value")) == "arrow_function" ||
                  type_of(field_of(node, "value")) == "function_expression")) {
        function = field_of(node, "value");
        name = text_of(field_of(node, "name"), source);
        first_line = line_of(node);
      }
      if (ts_node_is_null(function)) {
        return true;
      }
      const auto returned = js_decoder_types(function, file, source);
      if (returned.empty()) {
        return true;
      }
      claims_types.insert(returned.begin(), returned.end());
      if (const auto id = emitter.function_named(name, first_line, line_of(function)); !id.empty()) {
        decoders[name] = {id, returned};
      }
      return true;
    });
  }

  // Every property of a claims type is a claim this repo reads.
  for (const auto& claims_type : claims_types) {
    for (const auto& [member, claim] : js_properties(file.types.at(claims_type), source)) {
      emitter.claim(false, emitter.field_at(line_of(member), claim), claim);
    }
  }
  for (const auto& [name, decoder] : decoders) {
    for (const auto& claims_type : decoder.second) {
      for (const auto& property : js_properties(file.types.at(claims_type), source)) {
        emitter.emit(kDecoder, decoder.first, {}, property.second);
      }
    }
  }

  // Reads through a decoder: `const v = decodeToken(t)` (also behind await,
  // `?:`, `??`, `||`, and aliases `let w = v`), then `v.x` / `v?.x`. Only a
  // callee imported from the project's own code or a same-file decoder can be
  // one; resolution keeps only reads whose callee is a decoder whose type
  // declares `x`. Every parameter and declaration is a binding, so an inner
  // `payload` shadows an outer decoded one.
  if (file.local_imports.empty() && decoders.empty()) {
    return;
  }
  std::vector<std::unordered_map<std::string, std::set<std::string>>> scopes(1);
  const auto lookup = [&](const std::string& variable) -> const std::set<std::string>* {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      if (const auto found = scope->find(variable); found != scope->end()) {
        return &found->second;
      }
    }
    return nullptr;
  };
  const std::function<void(TSNode, std::set<std::string>&)> callees = [&](TSNode value, std::set<std::string>& out) {
    value = js_unwrap(value);
    const auto type = type_of(value);
    if (type == "call_expression" && type_of(field_of(value, "function")) == "identifier") {
      const auto callee = text_of(field_of(value, "function"), source);
      if (file.local_imports.contains(callee) || decoders.contains(callee)) {
        out.insert(callee);
      }
    } else if (type == "identifier") {
      if (const auto* known = lookup(text_of(value, source))) {
        out.insert(known->begin(), known->end());
      }
    } else if (type == "ternary_expression") {
      callees(field_of(value, "consequence"), out);
      callees(field_of(value, "alternative"), out);
    } else if (type == "binary_expression") {
      const auto operator_text = text_of(field_of(value, "operator"), source);
      if (operator_text == "??" || operator_text == "||") {
        callees(field_of(value, "left"), out);
        callees(field_of(value, "right"), out);
      }
    }
  };
  const std::function<void(TSNode)> reads = [&](TSNode node) {
    const auto type = type_of(node);
    const bool function_like = js_function_like(type);
    // `for (const payload of rows)` and `catch (payload)` bind in their own block.
    const bool block_binding = type == "for_in_statement" || type == "catch_clause";
    if (block_binding) {
      scopes.emplace_back();
      std::vector<std::string> names;
      js_declared_names(field_of(node, type == "catch_clause" ? "parameter" : "left"), source, names);
      for (const auto& name : names) {
        scopes.back()[name];
      }
    }
    if (function_like) {
      scopes.emplace_back();
      std::vector<std::string> parameters;
      js_declared_names(field_of(node, "parameters"), source, parameters);
      js_declared_names(field_of(node, "parameter"), source, parameters);  // `x => ...`
      for (const auto& parameter : parameters) {
        scopes.back()[parameter];
      }
    }
    if (type == "variable_declarator") {
      const auto pattern = field_of(node, "name");
      if (type_of(pattern) == "identifier") {
        std::set<std::string> found;
        callees(field_of(node, "value"), found);
        scopes.back()[text_of(pattern, source)] = std::move(found);
      } else {
        std::vector<std::string> names;
        js_declared_names(pattern, source, names);
        for (const auto& name : names) {
          scopes.back()[name].clear();
        }
      }
    } else if (type == "assignment_expression" && type_of(field_of(node, "left")) == "identifier") {
      const auto variable = text_of(field_of(node, "left"), source);
      std::set<std::string> found;
      callees(field_of(node, "right"), found);
      for (auto scope = scopes.rbegin(); scope != scopes.rend() && !found.empty(); ++scope) {
        if (const auto slot = scope->find(variable); slot != scope->end()) {
          slot->second.insert(found.begin(), found.end());
          break;
        }
      }
    } else if (type == "member_expression" && type_of(field_of(node, "object")) == "identifier" &&
               type_of(field_of(node, "property")) == "property_identifier") {
      if (const auto* known = lookup(text_of(field_of(node, "object"), source)); known != nullptr && !known->empty()) {
        const auto property = text_of(field_of(node, "property"), source);
        const auto scope = emitter.scope_at(line_of(node));
        for (const auto& callee : *known) {
          emitter.emit(kRead, scope, callee, property);
        }
      }
    }
    for (std::uint32_t index = 0; index < named_count(node); ++index) {
      reads(ts_node_named_child(node, index));
    }
    if (function_like) {
      scopes.pop_back();
    }
    if (block_binding) {
      scopes.pop_back();
    }
  };
  reads(root);
}

// --- Python ------------------------------------------------------------------

void python_claims(TSNode root, Emitter& emitter) {
  const auto source = emitter.source();
  std::unordered_set<std::string> modules;  // PyJWT's `jwt`, python-jose's `jwt`
  walk(root, [&](TSNode node) {
    const auto type = type_of(node);
    const bool plain = type == "import_statement";
    const bool from_jose = type == "import_from_statement" && text_of(field_of(node, "module_name"), source) == "jose";
    if (!plain && !from_jose) {
      return type == "module";
    }
    for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
      const char* field_name = ts_node_field_name_for_child(node, index);
      if (field_name == nullptr || std::string_view(field_name) != "name") {
        continue;
      }
      const auto imported = ts_node_child(node, index);
      if (type_of(imported) == "dotted_name" && text_of(imported, source) == "jwt") {
        modules.insert("jwt");
      } else if (type_of(imported) == "aliased_import" && text_of(field_of(imported, "name"), source) == "jwt") {
        modules.insert(text_of(field_of(imported, "alias"), source));
      }
    }
    return false;
  });
  if (modules.empty()) {
    return;
  }
  walk(root, [&](TSNode node) {
    if (type_of(node) != "call") {
      return true;
    }
    const auto function = field_of(node, "function");
    if (type_of(function) != "attribute" || !modules.contains(text_of(field_of(function, "object"), source)) ||
        text_of(field_of(function, "attribute"), source) != "encode") {
      return true;
    }
    const auto payload = named_child(field_of(node, "arguments"), 0);
    if (type_of(payload) != "dictionary") {
      return true;
    }
    const auto scope = emitter.scope_at(line_of(node));
    for (std::uint32_t index = 0; index < named_count(payload); ++index) {
      const auto pair = ts_node_named_child(payload, index);
      if (type_of(pair) == "pair") {
        if (const auto name = string_value(field_of(pair, "key"), source)) {
          emitter.claim(true, scope, *name);
        }
      }
    }
    return true;
  });
}

}  // namespace

void extract_claim_contracts(TSNode root, std::string_view language, const ExtractionContext& context,
                             const Fragment& fragment, std::vector<RawRelation>& out) {
  Emitter emitter(context, fragment, out);
  if (language == "kotlin") {
    kotlin_claims(root, emitter);
  } else if (language == "java") {
    java_claims(root, emitter);
  } else if (language == "go") {
    go_claims(root, emitter);
  } else if (language == "typescript" || language == "tsx" || language == "javascript") {
    js_claims(root, emitter);
  } else if (language == "python") {
    python_claims(root, emitter);
  }
}

bool is_claim_read_relation(std::string_view relation) { return relation == kDecoder || relation == kRead; }

std::vector<RawRelation> resolve_claim_reads(std::span<const RawRelation> raw_relations, const RelationScopes& scopes) {
  std::unordered_map<std::string, std::unordered_set<std::string>> decoder_claims;
  for (const auto& relation : raw_relations) {
    if (relation.relation == kDecoder) {
      decoder_claims[relation.source_id].insert(relation.context);
    }
  }
  std::vector<RawRelation> facts;
  if (decoder_claims.empty()) {
    return facts;
  }
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& relation : raw_relations) {
    if (relation.relation != kRead) {
      continue;
    }
    const auto decoder = resolve_scoped_name(scopes, relation.source_file, make_id(relation.target_label), true);
    const auto claims = decoder_claims.find(decoder);
    if (claims == decoder_claims.end() || !claims->second.contains(relation.context) ||
        !seen.emplace(relation.source_id, relation.context).second) {
      continue;
    }
    facts.push_back(RawRelation{
        .source_id = relation.source_id,
        .target_label = {},
        .relation = std::string(kUses),
        .context = "claim:" + relation.context,
        .source_file = relation.source_file,
        .allow_same_file = false,
    });
  }
  return facts;
}

}  // namespace cgraph
