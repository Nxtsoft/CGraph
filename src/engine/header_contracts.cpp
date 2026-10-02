#include "cgraph/header_contracts.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/data_contracts.hpp"
#include "cgraph/javascript_syntax.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

[[nodiscard]] bool is_lower_or_digit(char ch) {
  return std::islower(static_cast<unsigned char>(ch)) || std::isdigit(static_cast<unsigned char>(ch));
}

// True when `word` ends in `suffix` at a camelCase boundary (`agentRes` ends in
// `Res`; `Fres` does not, nor does `Latest` end in `Test`).
[[nodiscard]] bool camel_suffix(std::string_view word, std::string_view suffix) {
  return word.size() > suffix.size() && word.ends_with(suffix) && is_lower_or_digit(word[word.size() - suffix.size() - 1]);
}

// A field of `node`; null for a null node (a return outside any function has
// no enclosing function to name).
[[nodiscard]] TSNode field(const TSNode& node, std::string_view name) {
  return ts_node_is_null(node) ? TSNode{}
                               : ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
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

// A name that says it holds or builds request headers: exactly `headers` or
// `header`, or one ending in `Headers` / `Header` at a camelCase boundary
// (`authHeaders`, `getAuthHeaders`, `tenantHeader`, `doWithHeaders`) or in
// `_headers` / `_header`. Not a name that merely contains the word
// (`headerStyles`, `subheaderText`). A holder only records the keys that are
// header-shaped, so a `tableHeaders` of column titles records nothing.
[[nodiscard]] bool holds_headers(std::string_view name) {
  const auto lowered = lower(name);
  return lowered == "headers" || lowered == "header" || camel_suffix(name, "Headers") ||
         camel_suffix(name, "Header") || lowered.ends_with("_headers") || lowered.ends_with("_header");
}

// A word that names a response: `res`, `resp`, `response`, anything containing
// `response` (`nextResponse`, `ResponseEntity`, `JSONResponse`), or a camelCase
// `...Res` / `...Resp` (`agentRes`). A header set on a response or read from
// one is the server talking back, not the contract a client sends.
[[nodiscard]] bool is_response_word(std::string_view word) {
  const auto lowered = lower(word);
  return lowered == "res" || lowered == "resp" || lowered.find("response") != std::string::npos ||
         camel_suffix(word, "Res") || camel_suffix(word, "Resp");
}

// True when any identifier-like word of `text` names a response. `_` splits
// words too (`agent_res`).
[[nodiscard]] bool mentions_response(std::string_view text) {
  std::size_t start = 0;
  for (std::size_t index = 0; index <= text.size(); ++index) {
    if (index == text.size() || !std::isalnum(static_cast<unsigned char>(text[index]))) {
      if (index > start && is_response_word(text.substr(start, index - start))) {
        return true;
      }
      start = index + 1;
    }
  }
  return false;
}

// Records one header fact. `bound` marks a read the framework binds to a
// request itself (contracts.hpp kBoundHeaderRead): it provides even when no
// code calls the reading function.
void emit(std::vector<RawRelation>& out, const ExtractionContext& context, bool providing, const std::string& scope,
          const std::optional<std::string>& name, bool bound = false) {
  if (!name || !is_contract_header_name(*name)) {
    return;
  }
  if (is_test_source_path(context.relative_path)) {
    return;  // a test's fake server, or a request a test sends its own service
  }
  if (providing && scope.empty()) {
    return;  // a read outside any function: no handler provides it
  }
  out.push_back(RawRelation{
      .source_id = scope.empty() ? make_id(context.relative_path) : scope,
      .target_label = providing && bound ? std::string(kBoundHeaderRead) : std::string{},
      .relation = std::string(providing ? kProvides : kUses),
      .context = "header:" + *name,
      .source_file = context.source_file,
  });
}

// ---- JavaScript / TypeScript ------------------------------------------------

// What a file declares at module level that header reading looks up: string
// constants (the one hop a header name may take) and function parameter
// names (an object passed at an `extraHeaders` parameter is headers). Built
// once per file under a HeaderContractsFileScope.
struct JsFileIndex {
  bool built = false;
  std::unordered_map<std::string, std::optional<std::string>> constants;  // nullopt: declared, not a plain string
  std::unordered_map<std::string, std::vector<std::string>> functions;
};

thread_local JsFileIndex* current_js_index = nullptr;

// Through TypeScript's type-only wrappers (js_syntax::unwrap_expression) and `await`.
[[nodiscard]] TSNode js_value(TSNode node) {
  for (int guard = 0; guard < 16; ++guard) {
    node = js_syntax::unwrap_expression(node);
    if (type_of(node) != "await_expression" || ts_node_named_child_count(node) == 0) {
      break;
    }
    node = ts_node_named_child(node, 0);
  }
  return node;
}

// js_syntax::is_function_node, plus a generator expression (`function* () {}`).
[[nodiscard]] bool js_is_function(std::string_view type) {
  return js_syntax::is_function_node(type) || type == "generator_function";
}

// `'X-A'`, `"X-A"`, or a template literal with no substitution.
[[nodiscard]] std::optional<std::string> js_string(const TSNode& node, std::string_view source) {
  const auto type = type_of(node);
  if (type == "string" ||
      (type == "template_string" && ts_node_is_null(first_named_of_type(node, "template_substitution")))) {
    const auto text = text_of(node, source);
    return text.size() >= 2 ? std::optional<std::string>(text.substr(1, text.size() - 2)) : std::nullopt;
  }
  return std::nullopt;
}

void build_js_index(const TSNode& program, std::string_view source, JsFileIndex& index) {
  index.built = true;
  for (TSNode declaration : named_children(program)) {
    if (type_of(declaration) == "export_statement") {
      declaration = field(declaration, "declaration");
    }
    const auto type = type_of(declaration);
    if (type == "function_declaration" || type == "generator_function_declaration") {
      auto& parameters = index.functions[text_of(field(declaration, "name"), source)];
      parameters.clear();
      js_syntax::parameter_names(declaration, source, parameters);
      continue;
    }
    if (type != "lexical_declaration" && type != "variable_declaration") {
      continue;
    }
    const bool constant = ts_node_child_count(declaration) > 0 && text_of(ts_node_child(declaration, 0), source) == "const";
    for (const auto& declarator : named_children(declaration)) {
      if (type_of(declarator) != "variable_declarator") {
        continue;
      }
      const auto name = text_of(field(declarator, "name"), source);
      const TSNode value = js_value(field(declarator, "value"));
      if (js_syntax::is_function_node(type_of(value))) {
        auto& parameters = index.functions[name];
        parameters.clear();
        js_syntax::parameter_names(value, source, parameters);
      }
      // A second declaration of one name (`let` reassigned, a redeclared
      // `var`) is not a constant this file can read.
      const auto [slot, fresh] = index.constants.try_emplace(name, std::nullopt);
      slot->second = fresh && constant ? js_string(value, source) : std::nullopt;
    }
  }
}

// The current file's index: the scope's, built on first use, or a throwaway
// one when no scope is held.
[[nodiscard]] const JsFileIndex& js_index(const TSNode& from, std::string_view source, JsFileIndex& scratch) {
  JsFileIndex& index = current_js_index != nullptr ? *current_js_index : scratch;
  if (!index.built) {
    build_js_index(root_of(from), source, index);
  }
  return index;
}

// True when something between `use` and module scope binds `name` again: a
// parameter of an enclosing function, or a declaration directly in an
// enclosing block. The module constant is then not what `use` reads.
[[nodiscard]] bool js_shadowed(const TSNode& use, const std::string& name, std::string_view source) {
  for (TSNode scope = ts_node_parent(use); !ts_node_is_null(scope); scope = ts_node_parent(scope)) {
    const auto type = type_of(scope);
    if (type == "program") {
      return false;
    }
    if (js_is_function(type)) {
      std::vector<std::string> parameters;
      js_syntax::parameter_names(scope, source, parameters);
      if (std::ranges::find(parameters, name) != parameters.end()) {
        return true;
      }
    } else if (type == "statement_block" || type == "switch_body") {
      for (const auto& statement : named_children(scope)) {
        const auto statement_type = type_of(statement);
        if ((statement_type == "function_declaration" || statement_type == "class_declaration") &&
            text_of(field(statement, "name"), source) == name) {
          return true;
        }
        if (statement_type != "lexical_declaration" && statement_type != "variable_declaration") {
          continue;
        }
        for (const auto& declarator : named_children(statement)) {
          if (text_of(field(declarator, "name"), source) == name) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

[[nodiscard]] std::optional<std::string> js_name(const TSNode& value, std::string_view source) {
  const TSNode node = js_value(value);
  if (type_of(node) != "identifier") {
    return js_string(node, source);
  }
  const auto name = text_of(node, source);
  if (js_shadowed(node, name, source)) {
    return std::nullopt;
  }
  JsFileIndex scratch;
  const auto& index = js_index(node, source, scratch);
  const auto constant = index.constants.find(name);
  return constant == index.constants.end() ? std::nullopt : constant->second;
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

[[nodiscard]] std::string js_function_name(const TSNode& function, std::string_view source) {
  if (ts_node_is_null(function)) {
    return {};
  }
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

// A receiver that holds a request's headers: `headers`, `authHeaders`,
// `request.headers`, `c.req.raw.headers`, next/headers' `headers()`; never one
// that names a response (`agentRes.headers`), nor Elysia's `set.headers`, the
// response a handler is building.
[[nodiscard]] bool js_headers_receiver(const TSNode& value, std::string_view source) {
  const TSNode node = js_value(value);
  const auto type = type_of(node);
  if (type == "identifier") {
    const auto text = text_of(node, source);
    return holds_headers(text) && !mentions_response(text);
  }
  if (type == "member_expression") {
    const TSNode object = field(node, "object");
    return text_of(field(node, "property"), source) == "headers" && !mentions_response(text_of(object, source)) &&
           !(type_of(object) == "identifier" && text_of(object, source) == "set");
  }
  if (type == "call_expression") {  // next/headers: `headers().get('x-a')`
    return text_of(field(node, "function"), source) == "headers" &&
           named_children(field(node, "arguments")).empty();
  }
  return false;
}

// The call whose arguments `arguments` is, and its callee (`constructor` for `new`).
[[nodiscard]] TSNode js_callee(const TSNode& call) {
  const TSNode function = field(call, "function");
  return ts_node_is_null(function) ? field(call, "constructor") : function;
}

// True when `object` is the headers of a request being built.
[[nodiscard]] bool js_headers_object(const TSNode& object, std::string_view source, int depth = 0) {
  if (depth > 4) {
    return false;
  }
  TSNode child = object;
  TSNode parent = ts_node_parent(object);
  while (js_syntax::is_type_wrapper(type_of(parent)) || type_of(parent) == "ternary_expression" ||
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
    while (js_syntax::is_type_wrapper(type_of(ts_node_parent(options)))) {
      options = ts_node_parent(options);
    }
    const TSNode arguments = ts_node_parent(options);
    return type_of(arguments) != "arguments" ||
           !mentions_response(text_of(js_callee(ts_node_parent(arguments)), source));
  }
  if (type == "variable_declarator") {
    const auto name = text_of(field(parent, "name"), source);
    return holds_headers(name) && !mentions_response(name);
  }
  if (type == "assignment_expression" && ts_node_eq(field(parent, "right"), child)) {
    const TSNode left = field(parent, "left");
    if (type_of(left) == "identifier") {
      const auto name = text_of(left, source);
      return holds_headers(name) && !mentions_response(name);
    }
    return type_of(left) == "member_expression" && js_headers_receiver(left, source);
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
    std::size_t position = 0;
    for (const auto& argument : named_children(parent)) {
      if (ts_node_eq(argument, child)) {
        break;
      }
      ++position;
    }
    JsFileIndex scratch;
    const auto& index = js_index(call, source, scratch);
    const auto function = index.functions.find(text_of(callee, source));
    if (function == index.functions.end() || position >= function->second.size()) {
      return false;
    }
    const auto& parameter = function->second[position];
    return holds_headers(parameter) && !mentions_response(parameter);
  }
  if (type == "return_statement" || (js_is_function(type) && ts_node_eq(field(parent, "body"), child))) {
    const TSNode function = js_is_function(type) ? parent : js_enclosing_function(parent);
    const auto name = js_function_name(function, source);
    return holds_headers(name) && !mentions_response(name);
  }
  return false;
}

}  // namespace

struct HeaderContractsFileScope::Index {
  JsFileIndex js;
  JsFileIndex* previous = nullptr;
};

HeaderContractsFileScope::HeaderContractsFileScope() : index_(std::make_unique<Index>()) {
  index_->previous = current_js_index;
  current_js_index = &index_->js;
}

HeaderContractsFileScope::~HeaderContractsFileScope() { current_js_index = index_->previous; }

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
                         const Fragment& fragment, std::vector<RawRelation>& out) {
  const auto source = context.source;
  // A sender outside any function belongs to the module variable it helps
  // initialise when the extractor made a node for it, else to the file.
  const auto sender = [&] { return js_syntax::reading_scope_id(node, context, function_scope_id, fragment); };
  const auto type = type_of(node);
  if (type == "object") {
    if (!js_headers_object(node, source)) {
      return;
    }
    for (const auto& member : named_children(node)) {
      if (type_of(member) == "pair") {
        emit(out, context, false, sender(), js_key(field(member, "key"), source));
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
    emit(out, context, !written, written ? sender() : function_scope_id, js_name(field(node, "index"), source));
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
  if (method != "set" && method != "append" && method != "get" && method != "has" && method != "header") {
    return;
  }
  const TSNode receiver = field(callee, "object");
  const auto arguments = named_children(field(node, "arguments"));
  if ((method == "set" || method == "append") && arguments.size() == 2 && js_headers_receiver(receiver, source)) {
    emit(out, context, false, sender(), js_name(arguments[0], source));
  } else if ((method == "get" || method == "has") && arguments.size() == 1 && js_headers_receiver(receiver, source)) {
    emit(out, context, true, function_scope_id, js_name(arguments[0], source));
  } else if ((method == "header" || method == "get") && arguments.size() == 1) {
    // Hono's `c.req.header('x-a')`, Express's `req.get('x-a')` / `req.header('x-a')`.
    const auto receiver_text = lower(text_of(js_value(receiver), source));
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

// True when `name` is local to a function enclosing `use`: one of its
// parameters, or assigned anywhere in its body (Python makes such a name
// local to the whole function). Nested functions are their own scope.
[[nodiscard]] bool py_shadowed(const TSNode& use, const std::string& name, std::string_view source) {
  for (TSNode scope = ts_node_parent(use); !ts_node_is_null(scope); scope = ts_node_parent(scope)) {
    if (type_of(scope) != "function_definition") {
      continue;
    }
    for (const auto& parameter : named_children(field(scope, "parameters"))) {
      TSNode identifier = type_of(parameter) == "identifier" ? parameter : field(parameter, "name");
      if (ts_node_is_null(identifier)) {
        identifier = first_named_of_type(parameter, "identifier");
      }
      if (text_of(identifier, source) == name) {
        return true;
      }
    }
    std::vector<TSNode> pending{field(scope, "body")};
    while (!pending.empty()) {
      const TSNode node = pending.back();
      pending.pop_back();
      if (type_of(node) == "assignment" || type_of(node) == "augmented_assignment") {
        const TSNode left = field(node, "left");
        if (type_of(left) == "identifier" && text_of(left, source) == name) {
          return true;
        }
      }
      for (const auto& child : named_children(node)) {
        if (type_of(child) != "function_definition" && type_of(child) != "class_definition") {
          pending.push_back(child);
        }
      }
    }
  }
  return false;
}

[[nodiscard]] std::optional<std::string> py_const(const std::string& name, const TSNode& from,
                                                  std::string_view source) {
  std::optional<std::string> value;
  bool seen = false;
  for (TSNode statement : named_children(root_of(from))) {
    if (type_of(statement) == "expression_statement" && ts_node_named_child_count(statement) == 1) {
      statement = ts_node_named_child(statement, 0);
    }
    if (type_of(statement) == "assignment" && text_of(field(statement, "left"), source) == name) {
      if (seen) {
        return std::nullopt;  // assigned twice at module level: not a constant
      }
      seen = true;
      value = py_string(field(statement, "right"), source);
    }
  }
  return value;
}

[[nodiscard]] std::optional<std::string> py_name(const TSNode& node, std::string_view source) {
  if (type_of(node) != "identifier") {
    return py_string(node, source);
  }
  const auto name = text_of(node, source);
  if (py_shadowed(node, name, source)) {
    return std::nullopt;
  }
  return py_const(name, node, source);
}

[[nodiscard]] bool py_headers_receiver(const TSNode& node, std::string_view source) {
  if (type_of(node) == "identifier") {
    const auto text = text_of(node, source);
    return holds_headers(text) && !mentions_response(text);
  }
  return type_of(node) == "attribute" && text_of(field(node, "attribute"), source) == "headers" &&
         !mentions_response(text_of(field(node, "object"), source));
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
// by a `*_headers` function.
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
    return type_of(left) == "identifier" && holds_headers(left_text) && !mentions_response(left_text);
  }
  if (type == "return_statement") {
    const auto name = py_enclosing_function_name(parent, source);
    return holds_headers(name) && !mentions_response(name);
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
  const auto arguments = named_children(field(node, "arguments"));
  if (type_of(callee) == "attribute") {
    if (text_of(field(callee, "attribute"), source) == "get" && !arguments.empty() &&
        type_of(arguments[0]) != "keyword_argument" && py_headers_receiver(field(callee, "object"), source)) {
      emit(out, context, true, function_scope_id, py_name(arguments[0], source));
      return;
    }
    if (text_of(field(callee, "attribute"), source) != "Header") {
      return;
    }
  } else if (text_of(callee, source) != "Header") {
    return;
  }
  // FastAPI binds a `Header(...)` parameter from the request itself.
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
  emit(out, context, true, function_scope_id, name, /*bound=*/true);
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

// The names a call chain is spelled with, root first: the root identifier and
// each member and callee name (`ResponseEntity.status(400).header` is
// ResponseEntity, status). Arguments are not words of the chain:
// `.uri(responseUrl).header(...)` builds a request.
void kt_chain_words(TSNode node, std::string_view source, std::vector<std::string>& out, int depth = 0) {
  for (; depth < 64; ++depth) {
    const auto type = type_of(node);
    if (type == "simple_identifier") {
      out.insert(out.begin(), text_of(node, source));
      return;
    }
    if (type == "call_expression" || type == "parenthesized_expression") {
      node = ts_node_named_child(node, 0);
      continue;
    }
    if (type != "navigation_expression") {
      return;
    }
    const auto parts = named_children(node);
    if (parts.size() < 2) {
      return;
    }
    out.insert(out.begin(), text_of(first_named_of_type(parts.back(), "simple_identifier"), source));
    node = parts.front();
  }
}

// True when `call` in `node`'s function is Ktor's ApplicationCall: no
// parameter or local of the enclosing function is named `call` (it is the
// route lambda's implicit `call`), or the parameter named so is typed
// `ApplicationCall` (`fun handle(call: ApplicationCall)`).
[[nodiscard]] bool kt_call_is_application_call(const TSNode& node, std::string_view source) {
  TSNode function = ts_node_parent(node);
  while (!ts_node_is_null(function) && type_of(function) != "function_declaration") {
    function = ts_node_parent(function);
  }
  if (ts_node_is_null(function)) {
    return true;
  }
  for (const auto& parameter : named_children(first_named_of_type(function, "function_value_parameters"))) {
    if (type_of(parameter) == "parameter" &&
        text_of(first_named_of_type(parameter, "simple_identifier"), source) == "call") {
      return text_of(parameter, source).find("ApplicationCall") != std::string::npos;
    }
  }
  std::vector<TSNode> pending{first_named_of_type(function, "function_body")};
  while (!pending.empty()) {
    const TSNode current = pending.back();
    pending.pop_back();
    if (type_of(current) == "property_declaration" &&
        text_of(first_named_of_type(first_named_of_type(current, "variable_declaration"), "simple_identifier"),
                source) == "call") {
      return false;
    }
    for (const auto& child : named_children(current)) {
      pending.push_back(child);
    }
  }
  return true;
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
    // Spring binds an `@RequestHeader` parameter from the request itself.
    const TSNode invocation = first_named_of_type(node, "constructor_invocation");
    const auto annotation = text_of(first_named_of_type(invocation, "user_type"), source);
    if (annotation != "RequestHeader" && !annotation.ends_with(".RequestHeader")) {
      return;
    }
    for (const auto& argument : kt_arguments(first_named_of_type(invocation, "value_arguments"), source)) {
      if (argument.name.empty() || argument.name == "name" || argument.name == "value") {
        emit(out, context, true, function_scope_id, kt_name(argument.value, source), /*bound=*/true);
        return;
      }
    }
    return;
  }
  if (type != "call_expression") {
    return;
  }
  const TSNode callee = ts_node_named_child(node, 0);
  const auto callee_type = type_of(callee);
  std::string member;
  TSNode receiver{};
  if (callee_type == "simple_identifier") {
    member = text_of(callee, source);
  } else if (callee_type == "navigation_expression") {
    const auto parts = named_children(callee);
    if (parts.size() != 2 || type_of(parts[1]) != "navigation_suffix") {
      return;
    }
    member = text_of(first_named_of_type(parts[1], "simple_identifier"), source);
    receiver = parts[0];
  } else {
    return;
  }
  if (member != "header" && member != "append" && member != "addHeader" && member != "setHeader" &&
      member != "getHeader" && member != "getHeaders") {
    return;
  }
  const TSNode suffix = first_named_of_type(node, "call_suffix");
  const auto arguments = kt_arguments(first_named_of_type(suffix, "value_arguments"), source);
  if (arguments.empty()) {
    return;
  }
  if (ts_node_is_null(receiver)) {
    if (member == "header" && arguments.size() == 2) {
      // Ktor's request builder: `client.post(url) { header("X-A", v) }`.
      emit(out, context, false, function_scope_id, kt_name(arguments[0].value, source));
    } else if (member == "append" && arguments.size() == 2) {
      // `headers { append("X-A", v) }`.
      const TSNode outer = kt_lambda_call(node);
      if (!ts_node_is_null(outer) && text_of(ts_node_named_child(outer, 0), source) == "headers") {
        emit(out, context, false, function_scope_id, kt_name(arguments[0].value, source));
      }
    }
    return;
  }
  std::vector<std::string> words;
  kt_chain_words(receiver, source, words);
  if (words.empty()) {
    return;
  }
  if ((member == "header" || member == "addHeader" || member == "setHeader") && arguments.size() == 2) {
    // A request builder (`Request.Builder().addHeader`, `restClient.post().header`);
    // `ResponseEntity.status(...).header(...)` and `response.addHeader` answer.
    if (std::ranges::none_of(words, [](const std::string& word) { return is_response_word(word); })) {
      emit(out, context, false, function_scope_id, kt_name(arguments[0].value, source));
    }
  } else if ((member == "getHeader" || member == "header" || member == "getHeaders") && arguments.size() == 1 &&
             lower(words.back()).ends_with("request")) {
    // Ktor binds `call` (the ApplicationCall) to the handler: `call.request.header`.
    emit(out, context, true, function_scope_id, kt_name(arguments[0].value, source),
         /*bound=*/words.front() == "call" && kt_call_is_application_call(node, source));
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

[[nodiscard]] TSNode go_enclosing_function(const TSNode& node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    const auto type = type_of(parent);
    if (type == "function_declaration" || type == "method_declaration" || type == "func_literal") {
      return parent;
    }
  }
  return TSNode{};
}

// The declared type of `name` among the parameters of the function `node`
// sits in (`*gin.Context` for `c` in `func(c *gin.Context)`), else empty.
[[nodiscard]] std::string go_parameter_type(const TSNode& node, const std::string& name, std::string_view source) {
  for (const auto& parameter : named_children(field(go_enclosing_function(node), "parameters"))) {
    for (std::uint32_t index = 0; index < ts_node_named_child_count(parameter); ++index) {
      const TSNode child = ts_node_named_child(parameter, index);
      if (type_of(child) == "identifier" && text_of(child, source) == name) {
        return text_of(field(parameter, "type"), source);
      }
    }
  }
  return {};
}

// True inside a net/http handler, `func(w http.ResponseWriter, r *http.Request)`:
// the server binds the request it reads.
[[nodiscard]] bool go_in_http_handler(const TSNode& node, std::string_view source) {
  const TSNode function = go_enclosing_function(node);
  bool writer = false;
  bool request = false;
  for (const auto& parameter : named_children(field(function, "parameters"))) {
    const auto type = text_of(field(parameter, "type"), source);
    writer = writer || type == "http.ResponseWriter";
    request = request || type == "*http.Request";
  }
  return writer && request;
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
    const TSNode key = ts_node_named_child(parent, 0);
    return !ts_node_eq(key, child) && holds_headers(text_of(key, source));
  }
  if (type_of(parent) == "argument_list") {  // `c.doWithHeaders(..., map[...]...)`
    const TSNode callee = field(ts_node_parent(parent), "function");
    const TSNode name = type_of(callee) == "selector_expression" ? field(callee, "field") : callee;
    return holds_headers(text_of(name, source));
  }
  if (type_of(parent) != "expression_list") {
    return false;
  }
  const TSNode holder = ts_node_parent(parent);
  const auto holder_type = type_of(holder);
  std::string name;
  if (holder_type == "return_statement") {
    name = text_of(field(go_enclosing_function(holder), "name"), source);
  } else if (holder_type == "short_var_declaration" || holder_type == "assignment_statement") {
    const auto left = named_children(field(holder, "left"));
    if (left.size() != 1) {
      return false;
    }
    name = text_of(left[0], source);
  } else if (holder_type == "var_spec") {
    name = text_of(field(holder, "name"), source);
  }
  return holds_headers(name) && !mentions_response(name);
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
  if (method != "GetHeader" && method != "Set" && method != "Add" && method != "Get" && method != "Values") {
    return;
  }
  const TSNode operand = field(callee, "operand");
  const auto arguments = named_children(field(node, "arguments"));
  if (method == "GetHeader") {
    // gin binds the handler's `c *gin.Context`; another type's GetHeader may
    // read anything.
    if (arguments.size() == 1 && type_of(operand) == "identifier" &&
        go_parameter_type(node, text_of(operand, source), source) == "*gin.Context") {
      emit(out, context, true, function_scope_id, go_name(arguments[0], source), /*bound=*/true);
    }
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
    emit(out, context, true, function_scope_id, go_name(arguments[0], source), go_in_http_handler(node, source));
  }
}

}  // namespace cgraph
