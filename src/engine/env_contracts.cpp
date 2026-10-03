#include "cgraph/env_contracts.hpp"

#include "cgraph/javascript_syntax.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/spring_actuator.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace cgraph {
namespace {

using js_syntax::field_text;
using js_syntax::is_function_node;
using js_syntax::unwrap_expression;

constexpr std::string_view kUsesContract = "uses_contract";

// Bounds the `const a = b` / one-hop function chains a typed env object is
// followed through (and any cycle among them).
constexpr int kMaxBindingHops = 4;

// A field of `node`; null for a null node too, so lookups chain safely.
[[nodiscard]] TSNode field(const TSNode& node, std::string_view name) {
  if (ts_node_is_null(node)) {
    return TSNode{};
  }
  return ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
}

// The source text of `node`; empty for a null node (a missing field).
[[nodiscard]] std::string node_text(const TSNode& node, std::string_view source) {
  return ts_node_is_null(node) ? std::string{} : js_syntax::node_text(node, source);
}

[[nodiscard]] std::string_view type_of(const TSNode& node) {
  return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
}

void emit(std::vector<RawRelation>& out, const ExtractionContext& context, std::string source_id,
          std::string_view name) {
  if (!is_env_variable_name(name)) {
    return;
  }
  auto fact = "env:" + std::string(name);
  // One fact per reading symbol and name: a function reading BACKEND_URL ten
  // times consumes it once.
  const bool seen = std::ranges::any_of(out, [&](const RawRelation& relation) {
    return relation.relation == kUsesContract && relation.source_id == source_id && relation.context == fact;
  });
  if (seen) {
    return;
  }
  out.push_back(RawRelation{
      .source_id = std::move(source_id),
      .target_label = {},
      .relation = std::string(kUsesContract),
      .context = std::move(fact),
      .source_file = context.source_file,
  });
}

// The code a read outside any function belongs to: the file.
[[nodiscard]] std::string scope_or_file(const ExtractionContext& context, const std::string& function_scope_id) {
  return function_scope_id.empty() ? make_id(context.relative_path) : function_scope_id;
}

// A quoted literal's contents: `"X"`, `'X'`, `` `X` `` (Go raw). nullopt for a
// prefixed (`f"..."`), triple-quoted or interpolated string.
[[nodiscard]] std::optional<std::string> literal_contents(std::string_view text) {
  if (text.size() < 2) {
    return std::nullopt;
  }
  const char quote = text.front();
  if ((quote != '"' && quote != '\'' && quote != '`') || text.back() != quote) {
    return std::nullopt;
  }
  if (text.size() >= 6 && text.substr(0, 3) == std::string(3, quote)) {
    return std::nullopt;
  }
  return std::string(text.substr(1, text.size() - 2));
}

// Every `${NAME` placeholder in `text` whose NAME is env-shaped, by Spring's
// placeholder scan; NAME ends at `:` (a default follows). A placeholder nested
// in a default (`${A:${B}}`) is read too.
template <typename Emit>
void placeholders(std::string_view text, Emit&& each) {
  for (auto placeholder = find_spring_placeholder(text); placeholder.open != std::string_view::npos &&
                                                         placeholder.close != std::string_view::npos;
       placeholder = find_spring_placeholder(text, placeholder.open + 2)) {
    if (const auto name = placeholder.inner.substr(0, placeholder.inner.find(':')); is_env_shaped_name(name)) {
      each(name);
    }
  }
}

[[nodiscard]] TSNode named_child_of_type(const TSNode& node, std::string_view type) {
  if (ts_node_is_null(node)) {
    return TSNode{};
  }
  for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
    if (const TSNode child = ts_node_named_child(node, index); type_of(child) == type) {
      return child;
    }
  }
  return TSNode{};
}

// ---------------------------------------------------------------- JavaScript

[[nodiscard]] bool is_js_env_root(TSNode node, std::string_view source) {
  node = unwrap_expression(node);
  if (ts_node_is_null(node) || type_of(node) != "member_expression") {
    return false;
  }
  const auto text = node_text(node, source);
  return text == "process.env" || text == "import.meta.env" || text == "Bun.env";
}

// Calls `each(name)` for every name the binding pattern `node` (an
// identifier, a destructuring pattern, a parameter list, an import clause)
// binds at any depth, until `each` returns true; true when it did.
template <typename Each>
bool any_bound(const TSNode& node, std::string_view source, Each&& each) {
  if (ts_node_is_null(node)) {
    return false;
  }
  const auto type = type_of(node);
  if ((type == "identifier" || type == "shorthand_property_identifier_pattern") && each(node_text(node, source))) {
    return true;
  }
  // Only the binding side: `{ key: binding }`, `binding = default`, a TS
  // parameter's pattern (not its type or default).
  const char* only = type == "pair_pattern"                                            ? "value"
                     : type == "assignment_pattern" || type == "object_assignment_pattern" ? "left"
                     : type == "required_parameter" || type == "optional_parameter"   ? "pattern"
                                                                                      : nullptr;
  if (only != nullptr) {
    return any_bound(field(node, only), source, each);
  }
  for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
    const TSNode child = ts_node_named_child(node, index);
    if (type_of(child) != "type_annotation" && any_bound(child, source, each)) {
      return true;
    }
  }
  return false;
}

// True when the binding pattern `node` binds `name` at any depth.
[[nodiscard]] bool binds_parameter(const TSNode& node, std::string_view name, std::string_view source) {
  return any_bound(node, source, [&](const std::string& bound) { return bound == name; });
}

[[nodiscard]] bool function_binds(const TSNode& function, std::string_view name, std::string_view source) {
  for (const auto* key : {"parameters", "parameter"}) {
    if (const TSNode parameters = field(function, key); !ts_node_is_null(parameters) &&
                                                        binds_parameter(parameters, name, source)) {
      return true;
    }
  }
  // A named function expression binds its own name inside its body.
  return type_of(function) == "function_expression" && field_text(function, "name", source) == name;
}

[[nodiscard]] bool is_function_scope(std::string_view type) {
  return is_function_node(type) || type == "function" || type == "generator_function";
}

// Every name a `var` anywhere under `scope` (not inside a nested function)
// binds: `var` hoists to the enclosing function or the program.
void collect_hoisted(const TSNode& scope, std::string_view source, std::unordered_set<std::string>& names) {
  if (ts_node_is_null(scope)) {
    return;
  }
  for (std::uint32_t index = 0; index < ts_node_named_child_count(scope); ++index) {
    const TSNode child = ts_node_named_child(scope, index);
    const auto type = type_of(child);
    if (is_function_scope(type)) {
      continue;
    }
    if (type == "variable_declaration") {
      for (std::uint32_t slot = 0; slot < ts_node_named_child_count(child); ++slot) {
        any_bound(field(ts_node_named_child(child, slot), "name"), source, [&](std::string bound) {
          names.insert(std::move(bound));
          return false;
        });
      }
    }
    collect_hoisted(child, source, names);
  }
}

// What a statement directly inside a block or the program binds a name to.
enum class Bound { kOther, kConst, kFunction };

struct Binding {
  Bound bound = Bound::kOther;
  TSNode node{};  // the `const` declarator or function declaration
};

// Calls `each(name, binding)` for every name `statement` binds, in source
// order.
template <typename Each>
void statement_bindings(TSNode statement, std::string_view source, Each&& each) {
  if (type_of(statement) == "export_statement") {
    statement = field(statement, "declaration");
  }
  const auto type = type_of(statement);
  if (type == "function_declaration" || type == "generator_function_declaration") {
    each(field_text(statement, "name", source), Binding{.bound = Bound::kFunction, .node = statement});
    return;
  }
  if (type == "class_declaration" || type == "abstract_class_declaration" || type == "enum_declaration") {
    each(field_text(statement, "name", source), Binding{});
    return;
  }
  if (type == "import_statement") {
    any_bound(named_child_of_type(statement, "import_clause"), source, [&](std::string bound) {
      each(std::move(bound), Binding{});
      return false;
    });
    return;
  }
  if (type != "lexical_declaration" && type != "variable_declaration") {
    return;
  }
  const bool constant = type == "lexical_declaration" && ts_node_child_count(statement) > 0 &&
                        node_text(ts_node_child(statement, 0), source) == "const";
  for (std::uint32_t slot = 0; slot < ts_node_named_child_count(statement); ++slot) {
    const TSNode declarator = ts_node_named_child(statement, slot);
    if (type_of(declarator) != "variable_declarator") {
      continue;
    }
    const TSNode pattern = field(declarator, "name");
    // `let`, `var`, or a destructured `const { name }` proves nothing.
    const Binding binding = constant && type_of(pattern) == "identifier"
                                ? Binding{.bound = Bound::kConst, .node = declarator}
                                : Binding{};
    any_bound(pattern, source, [&](std::string bound) {
      each(std::move(bound), binding);
      return false;
    });
  }
}

// What each block-like scope of one file binds, built once per scope: the
// first binding of each name among its statements, and for a function body
// or the program the names its `var`s hoist.
struct EnvFileIndex {
  std::unordered_map<const void*, std::unordered_map<std::string, Binding>> blocks;
  std::unordered_map<const void*, std::unordered_set<std::string>> hoisted;
};

thread_local EnvFileIndex* current_env_index = nullptr;

// The statements directly in a block-like scope: a program or statement
// block's children, or every case's statements of a switch (one scope).
template <typename Each>
void scope_statements(const TSNode& scope, Each&& each) {
  for (std::uint32_t index = 0; index < ts_node_named_child_count(scope); ++index) {
    const TSNode child = ts_node_named_child(scope, index);
    const auto type = type_of(child);
    if (type_of(scope) != "switch_body") {
      each(child);
    } else if (type == "switch_case" || type == "switch_default") {
      const TSNode value = field(child, "value");
      for (std::uint32_t slot = 0; slot < ts_node_named_child_count(child); ++slot) {
        if (const TSNode statement = ts_node_named_child(child, slot); !ts_node_eq(statement, value)) {
          each(statement);
        }
      }
    }
  }
}

[[nodiscard]] const std::unordered_map<std::string, Binding>& block_bindings(const TSNode& scope,
                                                                            std::string_view source,
                                                                            EnvFileIndex& index) {
  const auto [slot, fresh] = index.blocks.try_emplace(scope.id);
  if (fresh) {
    scope_statements(scope, [&](const TSNode& statement) {
      statement_bindings(statement, source, [&](std::string name, const Binding& binding) {
        slot->second.try_emplace(std::move(name), binding);  // the first binding of a name decides
      });
    });
  }
  return slot->second;
}

[[nodiscard]] bool hoisted_var(const TSNode& scope, std::string_view name, std::string_view source,
                               EnvFileIndex& index) {
  if (ts_node_is_null(scope)) {
    return false;
  }
  const auto [slot, fresh] = index.hoisted.try_emplace(scope.id);
  if (fresh) {
    collect_hoisted(scope, source, slot->second);
  }
  return slot->second.contains(std::string(name));
}

// The `const name = …` declarator or `function name` declaration the
// identifier `name` at `from` refers to, innermost scope first. Any other
// binding of the name on the way out (a `let` or `var`, a parameter, a
// for-head, a `catch`, a class, an enum, an import, a destructured `const`)
// shadows whatever is further out and proves nothing: null. Null too when
// nothing in the file binds it.
[[nodiscard]] TSNode find_binding(const TSNode& from, std::string_view name, std::string_view source) {
  EnvFileIndex scratch;
  EnvFileIndex& index = current_env_index != nullptr ? *current_env_index : scratch;
  for (TSNode scope = ts_node_parent(from); !ts_node_is_null(scope); scope = ts_node_parent(scope)) {
    const auto scope_type = type_of(scope);
    if (is_function_scope(scope_type)) {
      if (function_binds(scope, name, source) || hoisted_var(field(scope, "body"), name, source, index)) {
        return TSNode{};
      }
      continue;
    }
    if (scope_type == "for_in_statement" || scope_type == "for_statement") {
      const TSNode head = field(scope, scope_type == "for_in_statement" ? "left" : "initializer");
      if (!ts_node_is_null(head) && binds_parameter(head, name, source)) {
        return TSNode{};
      }
      continue;
    }
    if (scope_type == "catch_clause") {
      if (const TSNode parameter = field(scope, "parameter");
          !ts_node_is_null(parameter) && binds_parameter(parameter, name, source)) {
        return TSNode{};
      }
      continue;
    }
    if (scope_type != "program" && scope_type != "statement_block" && scope_type != "switch_body") {
      continue;
    }
    const auto& bindings = block_bindings(scope, source, index);
    if (const auto found = bindings.find(std::string(name)); found != bindings.end()) {
      return found->second.bound == Bound::kOther ? TSNode{} : found->second.node;
    }
    if (scope_type == "program" && hoisted_var(scope, name, source, index)) {
      return TSNode{};
    }
  }
  return TSNode{};
}

[[nodiscard]] bool is_js_env_value(TSNode value, std::string_view source, int hops);

// Every value a function returns (`return x`, or an arrow's expression body),
// not looking into nested functions. Empty when it returns nothing readable.
void collect_returns(const TSNode& node, std::vector<TSNode>& out) {
  for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
    const TSNode child = ts_node_named_child(node, index);
    const auto type = type_of(child);
    if (is_function_node(type) || type == "function" || type == "class_declaration") {
      continue;
    }
    if (type == "return_statement") {
      out.push_back(ts_node_named_child_count(child) == 0 ? TSNode{} : ts_node_named_child(child, 0));
      continue;
    }
    collect_returns(child, out);
  }
}

// A function whose every return is a typed env object.
[[nodiscard]] bool returns_env(const TSNode& function, std::string_view source, int hops) {
  const TSNode body = field(function, "body");
  if (ts_node_is_null(body)) {
    return false;
  }
  std::vector<TSNode> returns;
  if (type_of(body) == "statement_block") {
    collect_returns(body, returns);
  } else {
    returns.push_back(body);  // `() => Value.Decode(schema, process.env)`
  }
  return !returns.empty() && std::ranges::all_of(returns, [&](const TSNode& returned) {
    return !ts_node_is_null(returned) && is_js_env_value(returned, source, hops);
  });
}

// True for a value whose keys are environment variable names: an env object
// itself, a call taking one as an argument (a schema validating the env), a
// `const` holding such a value, or a call of a same-file function returning one.
[[nodiscard]] bool is_js_env_value(TSNode value, std::string_view source, int hops) {
  value = unwrap_expression(value);
  if (ts_node_is_null(value) || hops > kMaxBindingHops) {
    return false;
  }
  if (type_of(value) == "await_expression" && ts_node_named_child_count(value) > 0) {
    value = unwrap_expression(ts_node_named_child(value, 0));
  }
  if (is_js_env_root(value, source)) {
    return true;
  }
  const auto type = type_of(value);
  if (type == "identifier") {
    const TSNode binding = find_binding(value, node_text(value, source), source);
    return type_of(binding) == "variable_declarator" && is_js_env_value(field(binding, "value"), source, hops + 1);
  }
  if (type != "call_expression") {
    return false;
  }
  const TSNode arguments = field(value, "arguments");
  for (std::uint32_t index = 0; !ts_node_is_null(arguments) && index < ts_node_named_child_count(arguments); ++index) {
    if (is_js_env_root(ts_node_named_child(arguments, index), source)) {
      return true;
    }
  }
  const TSNode callee = field(value, "function");
  if (type_of(callee) != "identifier") {
    return false;
  }
  TSNode function = find_binding(callee, node_text(callee, source), source);
  if (type_of(function) == "variable_declarator") {
    function = unwrap_expression(field(function, "value"));
  }
  const auto function_type = type_of(function);
  return (function_type == "function_declaration" || function_type == "arrow_function" ||
          function_type == "function_expression" || function_type == "function") &&
         returns_env(function, source, hops + 1);
}

// The env object a member or subscript reads from: true and `typed` false for
// `process.env` (any name counts), `typed` true for a typed env object (only
// env-shaped names count).
struct EnvObject {
  bool env = false;
  bool typed = false;
};

[[nodiscard]] EnvObject env_object(const TSNode& object, std::string_view source) {
  if (is_js_env_root(object, source)) {
    return {.env = true, .typed = false};
  }
  const TSNode bare = unwrap_expression(object);
  if (type_of(bare) == "identifier" && is_js_env_value(bare, source, 0)) {
    return {.env = true, .typed = true};
  }
  return {};
}

// `process.env.X = v`, `process.env.X += v`, `delete process.env.X`.
[[nodiscard]] bool is_write_target(const TSNode& node) {
  const TSNode parent = ts_node_parent(node);
  const auto type = type_of(parent);
  if (type == "assignment_expression" || type == "augmented_assignment_expression") {
    return ts_node_eq(field(parent, "left"), node);
  }
  if (type == "update_expression") {
    return true;
  }
  if (type == "unary_expression") {
    const TSNode op = field(parent, "operator");
    return !ts_node_is_null(op) && std::string_view(ts_node_type(op)) == "delete";
  }
  return false;
}

// `const { X, Y: y, Z = d } = <env value>`: the keys it reads.
void destructured_reads(const TSNode& pattern, bool typed, std::string_view source, std::vector<std::string>& names) {
  for (std::uint32_t index = 0; index < ts_node_named_child_count(pattern); ++index) {
    const TSNode entry = ts_node_named_child(pattern, index);
    const auto type = type_of(entry);
    TSNode key;
    if (type == "shorthand_property_identifier_pattern") {
      key = entry;
    } else if (type == "pair_pattern") {
      key = field(entry, "key");
    } else if (type == "object_assignment_pattern") {
      key = field(entry, "left");
    }
    const auto key_type = type_of(key);
    if (key_type != "shorthand_property_identifier_pattern" && key_type != "property_identifier" &&
        key_type != "string") {
      continue;
    }
    auto name = key_type == "string" ? literal_contents(node_text(key, source)).value_or(std::string{}) : node_text(key, source);
    if (!typed || is_env_shaped_name(name)) {
      names.push_back(std::move(name));
    }
  }
}

// ---------------------------------------------------------------- JVM

// `@Value("${X}")` / `@Value("\${X:d}")` (Kotlin escapes the `$`).
void value_annotation_reads(const TSNode& node, const ExtractionContext& context,
                            const std::string& function_scope_id, std::vector<RawRelation>& out) {
  const auto text = node_text(node, context.source);
  if (!text.starts_with("@Value(") && !text.starts_with("@org.springframework.beans.factory.annotation.Value(")) {
    return;
  }
  placeholders(text, [&](std::string_view name) { emit(out, context, scope_or_file(context, function_scope_id), name); });
}


// A string literal with no interpolation: Kotlin `"X"` (a `$` template is not a literal).
[[nodiscard]] std::optional<std::string> plain_literal(const TSNode& node, std::string_view source) {
  auto text = literal_contents(node_text(node, source));
  if (!text || text->find('$') != std::string::npos || text->find('\\') != std::string::npos) {
    return std::nullopt;
  }
  return text;
}

}  // namespace

struct EnvContractsFileScope::Index {
  EnvFileIndex env;
  EnvFileIndex* previous = nullptr;
};

EnvContractsFileScope::EnvContractsFileScope() : index_(std::make_unique<Index>()) {
  index_->previous = current_env_index;
  current_env_index = &index_->env;
}

EnvContractsFileScope::~EnvContractsFileScope() { current_env_index = index_->previous; }

bool is_env_variable_name(std::string_view name) {
  if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) {
    return false;
  }
  return std::ranges::all_of(name, [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
}

bool is_env_shaped_name(std::string_view name) {
  if (name.empty() || name.front() < 'A' || name.front() > 'Z') {
    return false;
  }
  return std::ranges::all_of(name, [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; });
}

void javascript_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                          const Fragment& fragment, std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  const auto& source = context.source;
  std::vector<std::string> names;
  if (type == "member_expression") {
    const TSNode property = field(node, "property");
    if (type_of(property) != "property_identifier") {
      return;
    }
    auto name = node_text(property, source);
    // Only an upper snake case member of a typed object is worth resolving its
    // object for: the binding search is the expensive part.
    const TSNode object = field(node, "object");
    if (!is_env_shaped_name(name) && !is_js_env_root(object, source)) {
      return;
    }
    const auto env = env_object(object, source);
    if (!env.env || is_write_target(node)) {
      return;
    }
    names.push_back(std::move(name));
  } else if (type == "subscript_expression") {
    const TSNode index = field(node, "index");
    if (type_of(index) != "string") {
      return;  // `process.env[key]`: no name to read
    }
    auto name = literal_contents(node_text(index, source));
    const auto env = name ? env_object(field(node, "object"), source) : EnvObject{};
    if (!env.env || is_write_target(node) || (env.typed && !is_env_shaped_name(*name))) {
      return;
    }
    names.push_back(std::move(*name));
  } else if (type == "variable_declarator") {
    const TSNode pattern = field(node, "name");
    const TSNode value = field(node, "value");
    if (type_of(pattern) != "object_pattern" || ts_node_is_null(value)) {
      return;
    }
    const bool root = is_js_env_root(value, source);
    if (!root && !is_js_env_value(value, source, 0)) {
      return;
    }
    destructured_reads(pattern, !root, source, names);
  } else {
    return;
  }
  if (names.empty()) {
    return;
  }
  const auto scope = js_syntax::reading_scope_id(node, context, function_scope_id, fragment);
  for (const auto& name : names) {
    emit(out, context, scope, name);
  }
}

void python_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                      std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  const auto& source = context.source;
  TSNode name_node;
  if (type == "subscript") {
    if (node_text(field(node, "value"), source) != "os.environ") {
      return;
    }
    const TSNode parent = ts_node_parent(node);
    const auto parent_type = type_of(parent);
    if (((parent_type == "assignment" || parent_type == "augmented_assignment") &&
         ts_node_eq(field(parent, "left"), node)) ||
        parent_type == "delete_statement" ||
        (parent_type == "expression_list" && type_of(ts_node_parent(parent)) == "delete_statement")) {
      return;  // `os.environ["X"] = v`, `del os.environ["X"]`: writes
    }
    name_node = field(node, "subscript");
  } else if (type == "call") {
    const auto callee = node_text(field(node, "function"), source);
    if (callee != "os.environ.get" && callee != "os.getenv") {
      return;
    }
    const TSNode arguments = field(node, "arguments");
    name_node = ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0
                    ? TSNode{}
                    : ts_node_named_child(arguments, 0);
  } else {
    return;
  }
  if (type_of(name_node) != "string") {
    return;
  }
  if (const auto name = literal_contents(node_text(name_node, source))) {
    emit(out, context, scope_or_file(context, function_scope_id), *name);
  }
}

void go_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                  std::vector<RawRelation>& out) {
  if (type_of(node) != "call_expression") {
    return;
  }
  const auto callee = node_text(field(node, "function"), context.source);
  if (callee != "os.Getenv" && callee != "os.LookupEnv") {
    return;
  }
  const TSNode arguments = field(node, "arguments");
  if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0) {
    return;
  }
  const TSNode first = ts_node_named_child(arguments, 0);
  const auto first_type = type_of(first);
  if (first_type != "interpreted_string_literal" && first_type != "raw_string_literal") {
    return;  // `os.Getenv(EnvClientID)`: a constant this reading does not follow
  }
  if (const auto name = literal_contents(node_text(first, context.source))) {
    emit(out, context, scope_or_file(context, function_scope_id), *name);
  }
}

void kotlin_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                      std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if (type == "annotation") {
    value_annotation_reads(node, context, function_scope_id, out);
    return;
  }
  if (type != "call_expression" || ts_node_named_child_count(node) == 0 ||
      node_text(ts_node_named_child(node, 0), context.source) != "System.getenv") {
    return;
  }
  const TSNode arguments = named_child_of_type(named_child_of_type(node, "call_suffix"), "value_arguments");
  const TSNode argument = ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0
                              ? TSNode{}
                              : ts_node_named_child(arguments, 0);
  const TSNode literal = ts_node_is_null(argument) ? TSNode{} : named_child_of_type(argument, "string_literal");
  if (ts_node_is_null(literal)) {
    return;
  }
  if (const auto name = plain_literal(literal, context.source)) {
    emit(out, context, scope_or_file(context, function_scope_id), *name);
  }
}

void java_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                    std::vector<RawRelation>& out) {
  const auto type = type_of(node);
  if (type == "annotation") {
    value_annotation_reads(node, context, function_scope_id, out);
    return;
  }
  if (type != "method_invocation" || node_text(field(node, "object"), context.source) != "System" ||
      node_text(field(node, "name"), context.source) != "getenv") {
    return;
  }
  const TSNode arguments = field(node, "arguments");
  const TSNode first = ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0
                           ? TSNode{}
                           : ts_node_named_child(arguments, 0);
  if (type_of(first) != "string_literal") {
    return;
  }
  if (const auto name = plain_literal(first, context.source)) {
    emit(out, context, scope_or_file(context, function_scope_id), *name);
  }
}

void append_spring_config_env_reads(const ExtractionContext& context, ExtractionResult& result) {
  const bool properties = context.relative_path.ends_with(".properties");
  const auto file_id = make_id(context.relative_path);
  std::string_view rest = context.source;
  while (!rest.empty()) {
    const auto newline = rest.find('\n');
    auto line = rest.substr(0, newline);
    rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
      continue;
    }
    if (line[first] == '#' || (properties && line[first] == '!')) {
      continue;  // a comment line
    }
    if (!properties) {
      line = strip_yaml_comment(line);
    }
    placeholders(line, [&](std::string_view name) { emit(result.raw_relations, context, file_id, name); });
  }
}

}  // namespace cgraph
