#include "cgraph/header_contracts.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

namespace {

constexpr std::string_view kProvides = "provides_contract";
constexpr std::string_view kUses = "uses_contract";

[[nodiscard]] std::string_view type_of(const TSNode& node) {
  return ts_node_is_null(node) ? std::string_view{} : std::string_view(ts_node_type(node));
}

[[nodiscard]] std::string text_of(const TSNode& node, std::string_view source) {
  if (ts_node_is_null(node)) {
    return {};
  }
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  if (start > end || end > source.size()) {
    return {};
  }
  return std::string(source.substr(start, end - start));
}

[[nodiscard]] std::string lower(std::string_view text) {
  std::string out(text);
  for (auto& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

[[nodiscard]] TSNode field(const TSNode& node, std::string_view name) {
  return ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
}

// Named children that are not comments.
[[nodiscard]] std::vector<TSNode> named_children(const TSNode& node) {
  std::vector<TSNode> out;
  if (ts_node_is_null(node)) {
    return out;
  }
  for (std::uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
    const TSNode child = ts_node_named_child(node, index);
    if (type_of(child).find("comment") == std::string_view::npos) {
      out.push_back(child);
    }
  }
  return out;
}

[[nodiscard]] TSNode first_named_of_type(const TSNode& node, std::string_view type) {
  for (const auto& child : named_children(node)) {
    if (type_of(child) == type) {
      return child;
    }
  }
  return TSNode{};
}

[[nodiscard]] TSNode root_of(TSNode node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(node)) {
    node = parent;
  }
  return node;
}

// A variable or function whose name says it holds or builds request headers:
// `headers`, `authHeaders`, `extraHeaders`, `tenantHeader`.
[[nodiscard]] bool names_headers(std::string_view name) { return lower(name).find("header") != std::string::npos; }

// Code that is about a response, not a request: an identifier in it is `res`,
// `resp` or `response`, or contains `response` (`nextResponse`,
// `ResponseEntity`, `JSONResponse`). A header set on a response or read from
// one is the server talking back, not the contract a client sends.
[[nodiscard]] bool mentions_response(std::string_view text) {
  std::string token;
  const auto check = [&token]() {
    const auto word = lower(token);
    token.clear();
    return word == "res" || word == "resp" || word == "response" || word.find("response") != std::string::npos;
  };
  for (const char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
      token.push_back(ch);
    } else if (!token.empty() && check()) {
      return true;
    }
  }
  return !token.empty() && check();
}

// A test source: `_test.go`, `*.test.ts`, `*.spec.ts`, `test_x.py`, `x_test.py`,
// `FooTest.kt`, or anything under a `test` / `tests` / `__tests__` / `e2e`
// directory. A handler there is a fake server standing in for another service
// (`httptest.NewServer` reading `X-Tenant-ID`), not this repo serving it.
[[nodiscard]] bool is_test_source(std::string_view relative_path) {
  std::string path = lower(relative_path);
  std::ranges::replace(path, '\\', '/');
  const auto slash = path.rfind('/');
  const std::string_view file = std::string_view(path).substr(slash == std::string::npos ? 0 : slash + 1);
  if (file.find(".test.") != std::string_view::npos || file.find(".spec.") != std::string_view::npos ||
      file.starts_with("test_") || file.find("_test.") != std::string_view::npos ||
      file.find("test.kt") != std::string_view::npos || file.find("tests.kt") != std::string_view::npos ||
      file.find("test.java") != std::string_view::npos) {
    return true;
  }
  const std::string directories = "/" + path.substr(0, slash == std::string::npos ? 0 : slash + 1);
  for (const std::string_view directory : {"/test/", "/tests/", "/__tests__/", "/e2e/"}) {
    if (directories.find(directory) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void emit(std::vector<RawRelation>& out, const ExtractionContext& context, bool providing, const std::string& scope,
          const std::optional<std::string>& name) {
  if (!name || !is_contract_header_name(*name)) {
    return;
  }
  if (providing && (scope.empty() || is_test_source(context.relative_path))) {
    return;  // a read outside any function, or a test's fake server: no handler provides it
  }
  out.push_back(RawRelation{
      .source_id = scope.empty() ? make_id(context.relative_path) : scope,
      .target_label = {},
      .relation = std::string(providing ? kProvides : kUses),
      .context = "header:" + *name,
      .source_file = context.source_file,
  });
}

// ---- JavaScript / TypeScript ------------------------------------------------

[[nodiscard]] bool js_is_wrapper(std::string_view type) {
  return type == "parenthesized_expression" || type == "as_expression" || type == "satisfies_expression" ||
         type == "non_null_expression" || type == "type_assertion" || type == "await_expression";
}

[[nodiscard]] TSNode js_unwrap(TSNode node) {
  while (js_is_wrapper(type_of(node)) && ts_node_named_child_count(node) > 0) {
    node = ts_node_named_child(node, 0);
  }
  return node;
}

// `'X-A'`, `"X-A"`, or a template literal with no substitution.
[[nodiscard]] std::optional<std::string> js_string(const TSNode& node, std::string_view source) {
  const auto type = type_of(node);
  if (type == "string") {
    const auto text = text_of(node, source);
    return text.size() >= 2 ? std::optional<std::string>(text.substr(1, text.size() - 2)) : std::nullopt;
  }
  if (type == "template_string" && ts_node_is_null(first_named_of_type(node, "template_substitution"))) {
    const auto text = text_of(node, source);
    return text.size() >= 2 ? std::optional<std::string>(text.substr(1, text.size() - 2)) : std::nullopt;
  }
  return std::nullopt;
}

// The string a module-level `const NAME = '...'` of this file holds: the one
// constant hop a header name may take.
[[nodiscard]] std::optional<std::string> js_const(const std::string& name, const TSNode& from,
                                                  std::string_view source) {
  const TSNode program = root_of(from);
  for (TSNode declaration : named_children(program)) {
    if (type_of(declaration) == "export_statement") {
      declaration = field(declaration, "declaration");
    }
    if (type_of(declaration) != "lexical_declaration" || ts_node_child_count(declaration) == 0 ||
        text_of(ts_node_child(declaration, 0), source) != "const") {
      continue;
    }
    for (const auto& declarator : named_children(declaration)) {
      if (type_of(declarator) == "variable_declarator" && text_of(field(declarator, "name"), source) == name) {
        return js_string(js_unwrap(field(declarator, "value")), source);
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> js_name(const TSNode& value, std::string_view source) {
  const TSNode node = js_unwrap(value);
  if (type_of(node) == "identifier") {
    return js_const(text_of(node, source), node, source);
  }
  return js_string(node, source);
}

[[nodiscard]] std::optional<std::string> js_key(const TSNode& key, std::string_view source) {
  const auto type = type_of(key);
  if (type == "computed_property_name" && ts_node_named_child_count(key) == 1) {
    return js_name(ts_node_named_child(key, 0), source);
  }
  if (type == "property_identifier") {
    return text_of(key, source);
  }
  return js_string(key, source);
}

[[nodiscard]] bool js_is_function(std::string_view type) {
  return type == "function_declaration" || type == "generator_function_declaration" || type == "function_expression" ||
         type == "function" || type == "arrow_function" || type == "method_definition" ||
         type == "generator_function";
}

[[nodiscard]] std::string js_function_name(const TSNode& function, std::string_view source) {
  if (const TSNode name = field(function, "name"); !ts_node_is_null(name)) {
    return text_of(name, source);
  }
  const TSNode parent = ts_node_parent(function);
  if (type_of(parent) == "variable_declarator") {
    return text_of(field(parent, "name"), source);
  }
  if (type_of(parent) == "pair") {
    return text_of(field(parent, "key"), source);
  }
  return {};
}

[[nodiscard]] TSNode js_enclosing_function(const TSNode& node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (js_is_function(type_of(parent))) {
      return parent;
    }
  }
  return TSNode{};
}

// The parameter names of the module-level function `name` of this file.
[[nodiscard]] std::vector<std::string> js_function_parameters(const std::string& name, const TSNode& from,
                                                              std::string_view source) {
  const TSNode program = root_of(from);
  for (TSNode declaration : named_children(program)) {
    if (type_of(declaration) == "export_statement") {
      declaration = field(declaration, "declaration");
    }
    TSNode function{};
    if (type_of(declaration) == "function_declaration" && text_of(field(declaration, "name"), source) == name) {
      function = declaration;
    } else if (type_of(declaration) == "lexical_declaration") {
      for (const auto& declarator : named_children(declaration)) {
        if (type_of(declarator) == "variable_declarator" && text_of(field(declarator, "name"), source) == name &&
            js_is_function(type_of(js_unwrap(field(declarator, "value"))))) {
          function = js_unwrap(field(declarator, "value"));
        }
      }
    }
    if (ts_node_is_null(function)) {
      continue;
    }
    std::vector<std::string> names;
    for (const auto& parameter : named_children(field(function, "parameters"))) {
      const TSNode pattern = field(parameter, "pattern");
      names.push_back(text_of(ts_node_is_null(pattern) ? parameter : pattern, source));
    }
    return names;
  }
  return {};
}

// A receiver that holds a request's headers: `headers`, `authHeaders`,
// `request.headers`, `c.req.raw.headers`, next/headers' `headers()`; never one
// that names a response.
[[nodiscard]] bool js_headers_receiver(const TSNode& value, std::string_view source) {
  const TSNode node = js_unwrap(value);
  const auto type = type_of(node);
  const auto text = text_of(node, source);
  if (mentions_response(text)) {
    return false;
  }
  if (type == "identifier") {
    return names_headers(text);
  }
  if (type == "member_expression") {
    return text_of(field(node, "property"), source) == "headers";
  }
  if (type == "call_expression") {  // next/headers: `headers().get('x-a')`
    return text_of(field(node, "function"), source) == "headers" &&
           named_children(field(node, "arguments")).empty();
  }
  return false;
}

// True when `object` is the headers of a request being built.
[[nodiscard]] bool js_headers_object(const TSNode& object, std::string_view source, int depth = 0) {
  if (depth > 4) {
    return false;
  }
  TSNode child = object;
  TSNode parent = ts_node_parent(object);
  while (js_is_wrapper(type_of(parent)) || type_of(parent) == "ternary_expression" ||
         type_of(parent) == "binary_expression") {
    child = parent;
    parent = ts_node_parent(parent);
  }
  const auto type = type_of(parent);
  if (type == "spread_element") {  // `{ ...(key ? { 'x-api-key': key } : {}) }`
    const TSNode outer = ts_node_parent(parent);
    return type_of(outer) == "object" && js_headers_object(outer, source, depth + 1);
  }
  if (type == "pair") {  // `{ headers: { ... } }`, not as a response's options
    if (lower(js_key(field(parent, "key"), source).value_or("")) != "headers") {
      return false;
    }
    TSNode options = ts_node_parent(parent);
    while (js_is_wrapper(type_of(ts_node_parent(options)))) {
      options = ts_node_parent(options);
    }
    const TSNode arguments = ts_node_parent(options);
    if (type_of(arguments) == "arguments") {
      const TSNode call = ts_node_parent(arguments);
      const TSNode callee = ts_node_is_null(field(call, "function")) ? field(call, "constructor") : field(call, "function");
      return !mentions_response(text_of(callee, source));
    }
    return true;
  }
  if (type == "variable_declarator") {
    const auto name = text_of(field(parent, "name"), source);
    return names_headers(name) && !mentions_response(name);
  }
  if (type == "assignment_expression" && ts_node_eq(field(parent, "right"), child)) {
    const TSNode left = field(parent, "left");
    const auto left_text = text_of(left, source);
    return !mentions_response(left_text) &&
           ((type_of(left) == "identifier" && names_headers(left_text)) ||
            (type_of(left) == "member_expression" && text_of(field(left, "property"), source) == "headers"));
  }
  if (type == "arguments") {
    const TSNode call = ts_node_parent(parent);
    if (type_of(call) == "new_expression") {
      return text_of(field(call, "constructor"), source) == "Headers";
    }
    const TSNode callee = field(call, "function");
    if (type_of(callee) != "identifier") {
      return false;
    }
    // `mlRequest(base, ..., { 'X-Webapp-Env': env })` into a function of this
    // file whose parameter there is `extraHeaders`.
    std::size_t index = 0;
    for (const auto& argument : named_children(parent)) {
      if (ts_node_eq(argument, child)) {
        break;
      }
      ++index;
    }
    const auto parameters = js_function_parameters(text_of(callee, source), call, source);
    return index < parameters.size() && names_headers(parameters[index]) && !mentions_response(parameters[index]);
  }
  if (type == "return_statement" || (js_is_function(type) && ts_node_eq(field(parent, "body"), child))) {
    const TSNode function = js_is_function(type) ? parent : js_enclosing_function(parent);
    const auto name = js_function_name(function, source);
    return names_headers(name) && !mentions_response(name);
  }
  return false;
}

}  // namespace

bool is_contract_header_name(std::string_view name) {
  if (name.empty() || name.size() > 128 || name.front() == '-' || name.back() == '-' ||
      name.find('-') == std::string_view::npos) {
    return false;
  }
  if (!std::ranges::all_of(name, [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-'; })) {
    return false;
  }
  return !is_standard_http_header(lower(name));
}

void js_header_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         std::vector<RawRelation>& out) {
  const auto source = context.source;
  const auto type = type_of(node);
  if (type == "object") {
    if (!js_headers_object(node, source)) {
      return;
    }
    for (const auto& member : named_children(node)) {
      if (type_of(member) == "pair") {
        emit(out, context, false, function_scope_id, js_key(field(member, "key"), source));
      }
    }
    return;
  }
  if (type == "subscript_expression") {
    if (!js_headers_receiver(field(node, "object"), source)) {
      return;
    }
    const TSNode parent = ts_node_parent(node);
    const bool written = type_of(parent) == "assignment_expression" && ts_node_eq(field(parent, "left"), node);
    emit(out, context, !written, function_scope_id, js_name(field(node, "index"), source));
    return;
  }
  if (type != "call_expression") {
    return;
  }
  const TSNode callee = field(node, "function");
  if (type_of(callee) != "member_expression") {
    return;
  }
  const auto method = text_of(field(callee, "property"), source);
  const TSNode receiver = field(callee, "object");
  const auto arguments = named_children(field(node, "arguments"));
  if ((method == "set" || method == "append") && arguments.size() == 2 && js_headers_receiver(receiver, source)) {
    emit(out, context, false, function_scope_id, js_name(arguments[0], source));
  } else if ((method == "get" || method == "has") && arguments.size() == 1 && js_headers_receiver(receiver, source)) {
    emit(out, context, true, function_scope_id, js_name(arguments[0], source));
  } else if ((method == "header" || method == "get") && arguments.size() == 1) {
    // Hono's `c.req.header('x-a')`, Express's `req.get('x-a')` / `req.header('x-a')`.
    const auto receiver_text = lower(text_of(js_unwrap(receiver), source));
    if (receiver_text == "req" || receiver_text == "request" || receiver_text.ends_with(".req") ||
        receiver_text.ends_with(".request")) {
      emit(out, context, true, function_scope_id, js_name(arguments[0], source));
    }
  }
}

namespace {

// ---- Python -----------------------------------------------------------------

[[nodiscard]] std::optional<std::string> py_string(const TSNode& node, std::string_view source) {
  if (type_of(node) != "string") {
    return std::nullopt;
  }
  std::string value;
  for (const auto& part : named_children(node)) {
    const auto part_type = type_of(part);
    if (part_type == "interpolation") {
      return std::nullopt;  // an f-string with a placeholder
    }
    if (part_type == "string_content") {
      value += text_of(part, source);
    }
  }
  return value;
}

[[nodiscard]] std::optional<std::string> py_const(const std::string& name, const TSNode& from,
                                                  std::string_view source) {
  for (TSNode statement : named_children(root_of(from))) {
    if (type_of(statement) == "expression_statement" && ts_node_named_child_count(statement) == 1) {
      statement = ts_node_named_child(statement, 0);
    }
    if (type_of(statement) == "assignment" && text_of(field(statement, "left"), source) == name) {
      return py_string(field(statement, "right"), source);
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> py_name(const TSNode& node, std::string_view source) {
  if (type_of(node) == "identifier") {
    return py_const(text_of(node, source), node, source);
  }
  return py_string(node, source);
}

[[nodiscard]] bool py_headers_receiver(const TSNode& node, std::string_view source) {
  const auto text = text_of(node, source);
  if (mentions_response(text)) {
    return false;
  }
  if (type_of(node) == "identifier") {
    return names_headers(text);
  }
  return type_of(node) == "attribute" && text_of(field(node, "attribute"), source) == "headers";
}

[[nodiscard]] std::string py_enclosing_function_name(const TSNode& node, std::string_view source) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (type_of(parent) == "function_definition") {
      return text_of(field(parent, "name"), source);
    }
  }
  return {};
}

// True when `dictionary` is a request's headers: `headers={...}` passed to a
// call that is not a response or an exception, `headers = {...}`, or returned
// by a `*header*` function.
[[nodiscard]] bool py_headers_dictionary(const TSNode& dictionary, std::string_view source) {
  const TSNode parent = ts_node_parent(dictionary);
  const auto type = type_of(parent);
  if (type == "keyword_argument") {
    if (text_of(field(parent, "name"), source) != "headers") {
      return false;
    }
    const TSNode call = ts_node_parent(ts_node_parent(parent));
    const auto callee = text_of(field(call, "function"), source);
    return !mentions_response(callee) && callee.find("Exception") == std::string::npos &&
           callee.find("Error") == std::string::npos;
  }
  if (type == "assignment" && ts_node_eq(field(parent, "right"), dictionary)) {
    const TSNode left = field(parent, "left");
    const auto left_text = text_of(left, source);
    return type_of(left) == "identifier" && names_headers(left_text) && !mentions_response(left_text);
  }
  if (type == "return_statement") {
    const auto name = py_enclosing_function_name(parent, source);
    return names_headers(name) && !mentions_response(name);
  }
  return false;
}

// The parameter a FastAPI `Header(...)` call declares, or null.
[[nodiscard]] TSNode py_header_parameter(const TSNode& call) {
  for (TSNode parent = ts_node_parent(call); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    const auto type = type_of(parent);
    if (type == "default_parameter" || type == "typed_default_parameter" || type == "typed_parameter") {
      return type_of(ts_node_parent(parent)) == "parameters" ? parent : TSNode{};
    }
    if (type == "parameters" || type == "function_definition" || type == "block" || type == "lambda") {
      break;
    }
  }
  return TSNode{};
}

}  // namespace

void python_header_contracts(const TSNode& node, const ExtractionContext& context,
                             const std::string& function_scope_id, std::vector<RawRelation>& out) {
  const auto source = context.source;
  const auto type = type_of(node);
  if (type == "dictionary") {
    if (!py_headers_dictionary(node, source)) {
      return;
    }
    for (const auto& pair : named_children(node)) {
      if (type_of(pair) == "pair") {
        emit(out, context, false, function_scope_id, py_string(field(pair, "key"), source));
      }
    }
    return;
  }
  if (type == "subscript") {
    if (!py_headers_receiver(field(node, "value"), source)) {
      return;
    }
    const TSNode parent = ts_node_parent(node);
    const bool written = type_of(parent) == "assignment" && ts_node_eq(field(parent, "left"), node);
    emit(out, context, !written, function_scope_id, py_name(field(node, "subscript"), source));
    return;
  }
  if (type != "call") {
    return;
  }
  const TSNode callee = field(node, "function");
  const auto callee_text = text_of(callee, source);
  const auto arguments = named_children(field(node, "arguments"));
  if (callee_text == "Header" || callee_text.ends_with(".Header")) {
    const TSNode parameter = py_header_parameter(node);
    if (ts_node_is_null(parameter)) {
      return;
    }
    std::optional<std::string> name;
    bool convert_underscores = true;
    for (const auto& argument : arguments) {
      if (type_of(argument) != "keyword_argument") {
        continue;
      }
      const auto keyword = text_of(field(argument, "name"), source);
      if (keyword == "alias") {
        name = py_name(field(argument, "value"), source);
        if (!name) {
          return;  // an alias this file cannot read: the parameter name is not the header
        }
      } else if (keyword == "convert_underscores") {
        convert_underscores = text_of(field(argument, "value"), source) != "False";
      }
    }
    if (!name) {
      // FastAPI reads `x_token: str = Header()` as header `x-token`.
      TSNode identifier = field(parameter, "name");
      if (ts_node_is_null(identifier)) {
        identifier = first_named_of_type(parameter, "identifier");
      }
      auto spelled = text_of(identifier, source);
      if (convert_underscores) {
        std::ranges::replace(spelled, '_', '-');
      }
      name = std::move(spelled);
    }
    emit(out, context, true, function_scope_id, name);
    return;
  }
  if (type_of(callee) == "attribute" && text_of(field(callee, "attribute"), source) == "get" && !arguments.empty() &&
      type_of(arguments[0]) != "keyword_argument" && py_headers_receiver(field(callee, "object"), source)) {
    emit(out, context, true, function_scope_id, py_name(arguments[0], source));
  }
}

namespace {

// ---- Kotlin -----------------------------------------------------------------

[[nodiscard]] std::optional<std::string> kt_string(const TSNode& node, std::string_view source) {
  if (type_of(node) != "string_literal") {
    return std::nullopt;
  }
  std::string value;
  for (const auto& part : named_children(node)) {
    if (type_of(part) != "string_content") {
      return std::nullopt;  // `"$x"` / `"${x}"`
    }
    value += text_of(part, source);
  }
  return value;
}

void kt_find_const(const TSNode& node, const std::string& name, std::string_view source,
                   std::vector<std::string>& values, int depth = 0) {
  if (depth > 64 || type_of(node) == "function_body") {
    return;
  }
  if (type_of(node) == "property_declaration") {
    const TSNode declaration = first_named_of_type(node, "variable_declaration");
    if (text_of(first_named_of_type(declaration, "simple_identifier"), source) == name) {
      if (auto value = kt_string(first_named_of_type(node, "string_literal"), source)) {
        values.push_back(std::move(*value));
      }
    }
    return;
  }
  for (const auto& child : named_children(node)) {
    kt_find_const(child, name, source, values, depth + 1);
  }
}

// `const val PAIRING_HEADER = "X-Passless-Pairing"` anywhere in this file
// outside a function (top level, an object, a companion), when one value.
[[nodiscard]] std::optional<std::string> kt_const(const std::string& name, const TSNode& from,
                                                  std::string_view source) {
  std::vector<std::string> values;
  kt_find_const(root_of(from), name, source, values);
  if (values.empty() || !std::ranges::all_of(values, [&](const auto& value) { return value == values.front(); })) {
    return std::nullopt;
  }
  return values.front();
}

[[nodiscard]] std::optional<std::string> kt_name(const TSNode& node, std::string_view source) {
  const auto type = type_of(node);
  if (type == "simple_identifier") {
    return kt_const(text_of(node, source), node, source);
  }
  if (type == "navigation_expression") {  // `LocalBroker.PAIRING_HEADER`
    const auto parts = named_children(node);
    const TSNode suffix = parts.empty() ? TSNode{} : parts.back();
    return kt_const(text_of(first_named_of_type(suffix, "simple_identifier"), source), node, source);
  }
  return kt_string(node, source);
}

struct KtArgument {
  std::string name;  // empty when positional
  TSNode value{};
};

[[nodiscard]] std::vector<KtArgument> kt_arguments(const TSNode& value_arguments, std::string_view source) {
  std::vector<KtArgument> out;
  for (const auto& argument : named_children(value_arguments)) {
    if (type_of(argument) != "value_argument") {
      continue;
    }
    const auto parts = named_children(argument);
    if (parts.empty()) {
      continue;
    }
    bool named = false;
    for (std::uint32_t index = 0; index < ts_node_child_count(argument); ++index) {
      const TSNode token = ts_node_child(argument, index);
      if (!ts_node_is_named(token) && text_of(token, source) == "=") {
        named = true;
      }
    }
    if (named && parts.size() >= 2) {
      out.push_back({.name = text_of(parts.front(), source), .value = parts.back()});
    } else {
      out.push_back({.name = {}, .value = parts.back()});
    }
  }
  return out;
}

// The call whose trailing lambda `node` sits in, or null.
[[nodiscard]] TSNode kt_lambda_call(const TSNode& node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (type_of(parent) == "annotated_lambda") {
      const TSNode suffix = ts_node_parent(parent);
      return type_of(suffix) == "call_suffix" ? ts_node_parent(suffix) : TSNode{};
    }
    if (type_of(parent) == "function_body") {
      break;
    }
  }
  return TSNode{};
}

}  // namespace

void kotlin_header_contracts(const TSNode& node, const ExtractionContext& context,
                             const std::string& function_scope_id, std::vector<RawRelation>& out) {
  const auto source = context.source;
  const auto type = type_of(node);
  if (type == "annotation") {
    const TSNode invocation = first_named_of_type(node, "constructor_invocation");
    const auto annotation = text_of(first_named_of_type(invocation, "user_type"), source);
    if (annotation != "RequestHeader" && !annotation.ends_with(".RequestHeader")) {
      return;
    }
    for (const auto& argument : kt_arguments(first_named_of_type(invocation, "value_arguments"), source)) {
      if (argument.name.empty() || argument.name == "name" || argument.name == "value") {
        emit(out, context, true, function_scope_id, kt_name(argument.value, source));
        return;
      }
    }
    return;
  }
  if (type != "call_expression") {
    return;
  }
  const TSNode callee = ts_node_named_child(node, 0);
  const TSNode suffix = first_named_of_type(node, "call_suffix");
  const auto arguments = kt_arguments(first_named_of_type(suffix, "value_arguments"), source);
  if (arguments.empty()) {
    return;
  }
  if (type_of(callee) == "simple_identifier") {
    const auto name = text_of(callee, source);
    if (name == "header" && arguments.size() == 2) {
      // Ktor's request builder: `client.post(url) { header("X-A", v) }`.
      emit(out, context, false, function_scope_id, kt_name(arguments[0].value, source));
    } else if (name == "append" && arguments.size() == 2) {
      // `headers { append("X-A", v) }`.
      const TSNode outer = kt_lambda_call(node);
      if (!ts_node_is_null(outer) && text_of(ts_node_named_child(outer, 0), source) == "headers") {
        emit(out, context, false, function_scope_id, kt_name(arguments[0].value, source));
      }
    }
    return;
  }
  if (type_of(callee) != "navigation_expression") {
    return;
  }
  const auto parts = named_children(callee);
  if (parts.size() != 2 || type_of(parts[1]) != "navigation_suffix") {
    return;
  }
  const auto member = text_of(first_named_of_type(parts[1], "simple_identifier"), source);
  const auto receiver = text_of(parts[0], source);
  if ((member == "header" || member == "addHeader" || member == "setHeader") && arguments.size() == 2) {
    // A request builder (`Request.Builder().addHeader`, `restClient.post().header`);
    // `ResponseEntity.status(...).header(...)` and `response.addHeader` answer.
    if (!mentions_response(receiver)) {
      emit(out, context, false, function_scope_id, kt_name(arguments[0].value, source));
    }
  } else if ((member == "getHeader" || member == "header" || member == "getHeaders") && arguments.size() == 1) {
    const auto lowered = lower(receiver);
    if (lowered == "request" || lowered.ends_with(".request") || lowered.ends_with("request")) {
      emit(out, context, true, function_scope_id, kt_name(arguments[0].value, source));
    }
  }
}

namespace {

// ---- Go ---------------------------------------------------------------------

[[nodiscard]] std::optional<std::string> go_string(const TSNode& node, std::string_view source) {
  const auto type = type_of(node);
  if (type == "interpreted_string_literal") {
    std::string value;
    for (const auto& part : named_children(node)) {
      if (type_of(part) != "interpreted_string_literal_content") {
        return std::nullopt;  // an escape sequence: not a header name
      }
      value += text_of(part, source);
    }
    return value;
  }
  if (type == "raw_string_literal") {
    const auto text = text_of(node, source);
    return text.size() >= 2 ? std::optional<std::string>(text.substr(1, text.size() - 2)) : std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> go_const(const std::string& name, const TSNode& from,
                                                  std::string_view source) {
  for (const auto& declaration : named_children(root_of(from))) {
    if (type_of(declaration) != "const_declaration" && type_of(declaration) != "var_declaration") {
      continue;
    }
    std::vector<TSNode> specs = named_children(declaration);
    if (specs.size() == 1 && type_of(specs[0]) == "var_spec_list") {
      specs = named_children(specs[0]);
    }
    for (const auto& spec : specs) {
      if (text_of(field(spec, "name"), source) != name) {
        continue;
      }
      const auto values = named_children(field(spec, "value"));
      return values.size() == 1 ? go_string(values[0], source) : std::nullopt;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> go_name(const TSNode& node, std::string_view source) {
  if (type_of(node) == "identifier") {
    return go_const(text_of(node, source), node, source);
  }
  return go_string(node, source);
}

[[nodiscard]] std::string go_function_name(const TSNode& node, std::string_view source) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (type_of(parent) == "function_declaration" || type_of(parent) == "method_declaration") {
      return text_of(field(parent, "name"), source);
    }
    if (type_of(parent) == "func_literal") {
      return {};
    }
  }
  return {};
}

// True when a `map[string]...{...}` literal is a request's headers.
[[nodiscard]] bool go_headers_map(const TSNode& literal, std::string_view source) {
  TSNode child = literal;
  TSNode parent = ts_node_parent(literal);
  if (type_of(parent) == "unary_expression") {
    child = parent;
    parent = ts_node_parent(parent);
  }
  if (type_of(parent) == "literal_element") {
    child = parent;
    parent = ts_node_parent(parent);
  }
  if (type_of(parent) == "keyed_element") {  // `Request{Header: map[...]...}`
    const auto key = text_of(ts_node_named_child(parent, 0), source);
    return !ts_node_eq(ts_node_named_child(parent, 0), child) && names_headers(key);
  }
  if (type_of(parent) == "argument_list") {  // `c.doWithHeaders(..., map[...]...)`
    const TSNode callee = field(ts_node_parent(parent), "function");
    const TSNode name = type_of(callee) == "selector_expression" ? field(callee, "field") : callee;
    return names_headers(text_of(name, source));
  }
  if (type_of(parent) != "expression_list") {
    return false;
  }
  const TSNode holder = ts_node_parent(parent);
  const auto holder_type = type_of(holder);
  if (holder_type == "return_statement") {
    const auto name = go_function_name(holder, source);
    return names_headers(name) && !mentions_response(name);
  }
  if (holder_type == "short_var_declaration" || holder_type == "assignment_statement") {
    const auto left = named_children(field(holder, "left"));
    if (left.size() != 1) {
      return false;
    }
    const auto name = text_of(left[0], source);
    return names_headers(name) && !mentions_response(name);
  }
  if (holder_type == "var_spec") {
    const auto name = text_of(field(holder, "name"), source);
    return names_headers(name) && !mentions_response(name);
  }
  return false;
}

}  // namespace

void go_header_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         std::vector<RawRelation>& out) {
  const auto source = context.source;
  const auto type = type_of(node);
  if (type == "composite_literal") {
    const TSNode literal_type = field(node, "type");
    bool headers = false;
    if (text_of(literal_type, source) == "http.Header") {
      headers = true;
    } else if (type_of(literal_type) == "map_type" && text_of(field(literal_type, "key"), source) == "string") {
      headers = go_headers_map(node, source);
    }
    if (!headers) {
      return;
    }
    for (const auto& element : named_children(field(node, "body"))) {
      if (type_of(element) != "keyed_element") {
        continue;
      }
      TSNode key = ts_node_named_child(element, 0);
      if (type_of(key) == "literal_element" && ts_node_named_child_count(key) == 1) {
        key = ts_node_named_child(key, 0);
      }
      emit(out, context, false, function_scope_id, go_name(key, source));
    }
    return;
  }
  if (type != "call_expression") {
    return;
  }
  const TSNode callee = field(node, "function");
  if (type_of(callee) != "selector_expression") {
    return;
  }
  const auto method = text_of(field(callee, "field"), source);
  const TSNode operand = field(callee, "operand");
  const auto arguments = named_children(field(node, "arguments"));
  if (method == "GetHeader" && arguments.size() == 1) {  // gin
    emit(out, context, true, function_scope_id, go_name(arguments[0], source));
    return;
  }
  // `req.Header.Set(...)`: the Header field of a request. A ResponseWriter's
  // headers are a call (`w.Header().Set`), and a response is named so.
  if (type_of(operand) != "selector_expression" || text_of(field(operand, "field"), source) != "Header" ||
      mentions_response(text_of(field(operand, "operand"), source))) {
    return;
  }
  if ((method == "Set" || method == "Add") && arguments.size() == 2) {
    emit(out, context, false, function_scope_id, go_name(arguments[0], source));
  } else if ((method == "Get" || method == "Values") && arguments.size() == 1) {
    emit(out, context, true, function_scope_id, go_name(arguments[0], source));
  }
}

}  // namespace cgraph
