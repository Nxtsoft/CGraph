#include "cgraph/http_consumers.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/javascript_syntax.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cgraph {

using namespace js_syntax;

namespace {

// ---- HTTP consumers (contracts.hpp: http_call / http_wrapper / http_call_args)

[[nodiscard]] TSNode program_of(TSNode node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(node)) {
    node = parent;
  }
  return node;
}

// Calls `visit` on every named descendant of `node` (not `node` itself). With
// `skip_functions`, a nested function's subtree is not entered: its calls run
// in another scope.
template <class Visit>
void visit_named_descendants(const TSNode& node, bool skip_functions, Visit&& visit) {
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode child = ts_node_named_child(node, index);
    if (skip_functions && is_function_node(ts_node_type(child))) {
      continue;
    }
    visit(child);
    visit_named_descendants(child, skip_functions, visit);
  }
}

// The value of a module-level `const NAME = ...` in this file (through casts),
// or null. Base URLs are spelled this way (`const base = \`${API_URL}/api/v1\``)
// and a wrapper's template inlines them.
[[nodiscard]] TSNode module_const_value(const TSNode& from, std::string_view name, std::string_view source) {
  const TSNode program = program_of(from);
  const auto count = ts_node_named_child_count(program);
  for (std::uint32_t index = 0; index < count; ++index) {
    TSNode statement = ts_node_named_child(program, index);
    if (std::string_view(ts_node_type(statement)) == "export_statement") {
      statement = ts_node_child_by_field_name(statement, "declaration", 11);
      if (ts_node_is_null(statement)) {
        continue;
      }
    }
    const std::string_view type = ts_node_type(statement);
    if (type != "lexical_declaration" && type != "variable_declaration") {
      continue;
    }
    const auto declarators = ts_node_named_child_count(statement);
    for (std::uint32_t d = 0; d < declarators; ++d) {
      const TSNode declarator = ts_node_named_child(statement, d);
      if (std::string_view(ts_node_type(declarator)) == "variable_declarator" &&
          field_text(declarator, "name", source) == name) {
        return unwrap_expression(ts_node_child_by_field_name(declarator, "value", 5));
      }
    }
  }
  return TSNode{};
}

// A module-level function of this file by name: `function f() {}`, `const f =
// () => ...` or `const f = function () {}`. Null for anything else, including
// an import: its body is not in this file.
[[nodiscard]] TSNode module_function(const TSNode& from, std::string_view name, std::string_view source) {
  const TSNode program = program_of(from);
  const auto count = ts_node_named_child_count(program);
  for (std::uint32_t index = 0; index < count; ++index) {
    TSNode statement = ts_node_named_child(program, index);
    if (std::string_view(ts_node_type(statement)) == "export_statement") {
      statement = ts_node_child_by_field_name(statement, "declaration", 11);
      if (ts_node_is_null(statement)) {
        continue;
      }
    }
    const std::string_view type = ts_node_type(statement);
    if (type == "function_declaration" && field_text(statement, "name", source) == name) {
      return statement;
    }
  }
  const TSNode value = module_const_value(from, name, source);
  return !ts_node_is_null(value) && is_function_value(value) ? value : TSNode{};
}

// The body of the class `from` sits in, or null.
[[nodiscard]] TSNode enclosing_class_body(const TSNode& from) {
  for (TSNode ancestor = ts_node_parent(from); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    if (std::string_view(ts_node_type(ancestor)) == "class_body") {
      return ancestor;
    }
  }
  return TSNode{};
}

// The method `name` of a class body: `name(...) {}` or a function-valued field
// `name = (...) => {}`. Null when the class declares no such member.
[[nodiscard]] TSNode class_method(const TSNode& body, std::string_view name, std::string_view source) {
  const auto count = ts_node_named_child_count(body);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(body, index);
    const std::string_view type = ts_node_type(member);
    if (type == "method_definition" && field_text(member, "name", source) == name) {
      return member;
    }
    if ((type == "public_field_definition" || type == "field_definition") && field_text(member, "name", source) == name) {
      const TSNode value = unwrap_expression(ts_node_child_by_field_name(member, "value", 5));
      return !ts_node_is_null(value) && is_function_value(value) ? value : TSNode{};
    }
  }
  return TSNode{};
}

// What `this.<name>` holds in a class: its field initializer, or the value of
// the one `this.<name> = value` assignment in the class (a constructor
// assigning an axios instance). Null when the class never sets it, sets it
// more than once, or updates it in place: which value a request sees is then
// not knowable from here.
[[nodiscard]] TSNode class_member_value(const TSNode& body, std::string_view name, std::string_view source) {
  TSNode value{};
  int writes = 0;
  const auto count = ts_node_named_child_count(body);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(body, index);
    const std::string_view type = ts_node_type(member);
    if ((type == "public_field_definition" || type == "field_definition") && field_text(member, "name", source) == name) {
      if (const TSNode initial = ts_node_child_by_field_name(member, "value", 5); !ts_node_is_null(initial)) {
        value = initial;
        ++writes;
      }
    }
  }
  visit_named_descendants(body, false, [&](const TSNode& node) {
    const std::string_view type = ts_node_type(node);
    if (type != "assignment_expression" && type != "augmented_assignment_expression") {
      return;
    }
    const TSNode left = unwrap_expression(ts_node_child_by_field_name(node, "left", 4));
    if (ts_node_is_null(left) || std::string_view(ts_node_type(left)) != "member_expression" ||
        field_text(left, "property", source) != name) {
      return;
    }
    const TSNode object = unwrap_expression(ts_node_child_by_field_name(left, "object", 6));
    if (ts_node_is_null(object) || std::string_view(ts_node_type(object)) != "this") {
      return;
    }
    value = ts_node_child_by_field_name(node, "right", 5);
    writes += type == "assignment_expression" ? 1 : 2;
  });
  return writes == 1 ? unwrap_expression(value) : TSNode{};
}

// The block-scoped declaration that binds `name` where `use` reads it: the
// nearest enclosing block declaring it directly. Empty when a function
// parameter binds the name first, or no enclosing block declares it (it is a
// module-level name, an import, or a global).
// True when a parameter of `function` binds `name`, destructured ones
// included (`({ apiUrl }) => ...` shadows an outer `apiUrl`).
[[nodiscard]] bool binds_pattern(const TSNode& node, std::string_view name, std::string_view source) {
  const std::string_view type = ts_node_type(node);
  if (type == "identifier" || type == "shorthand_property_identifier_pattern") {
    return node_text(node, source) == name;
  }
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode child = ts_node_named_child(node, index);
    // A type annotation, a default value or a renamed key names other things;
    // a renamed key's value (`{ data: url }`) is the name it binds.
    const char* field = ts_node_field_name_for_named_child(node, index);
    const std::string_view field_name = field == nullptr ? std::string_view{} : std::string_view(field);
    if (std::string_view(ts_node_type(child)) == "type_annotation" || (field_name == "value" && type != "pair_pattern") ||
        field_name == "right" || field_name == "key" || field_name == "type") {
      continue;
    }
    if (binds_pattern(child, name, source)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool binds_parameter(const TSNode& function, std::string_view name, std::string_view source) {
  TSNode parameters = ts_node_child_by_field_name(function, "parameters", 10);
  if (ts_node_is_null(parameters)) {
    parameters = ts_node_child_by_field_name(function, "parameter", 9);
  }
  return !ts_node_is_null(parameters) && binds_pattern(parameters, name, source);
}

struct LocalBinding {
  TSNode declarator;
  TSNode scope;
  bool constant = false;
  bool opaque = false;  // bound by a loop header or a `catch`: a value per iteration or thrown, never readable
};

// True when a `for (... of/in ...)`, a `for (let ...;;)` header or a `catch
// (e)` clause binds `name` for the code inside it.
[[nodiscard]] bool header_binds(const TSNode& statement, std::string_view name, std::string_view source) {
  const std::string_view type = ts_node_type(statement);
  if (type == "for_in_statement") {
    const TSNode left = ts_node_child_by_field_name(statement, "left", 4);
    return !ts_node_is_null(left) && binds_pattern(left, name, source);
  }
  if (type == "for_statement") {
    const TSNode initializer = ts_node_child_by_field_name(statement, "initializer", 11);
    if (ts_node_is_null(initializer)) {
      return false;
    }
    const auto count = ts_node_named_child_count(initializer);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode declarator = ts_node_named_child(initializer, index);
      if (std::string_view(ts_node_type(declarator)) == "variable_declarator" &&
          binds_pattern(ts_node_child_by_field_name(declarator, "name", 4), name, source)) {
        return true;
      }
    }
    return false;
  }
  if (type == "catch_clause") {
    const TSNode parameter = ts_node_child_by_field_name(statement, "parameter", 9);
    return !ts_node_is_null(parameter) && binds_pattern(parameter, name, source);
  }
  return false;
}

[[nodiscard]] std::optional<LocalBinding> local_binding(const TSNode& use, std::string_view name, std::string_view source) {
  for (TSNode ancestor = ts_node_parent(use); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    const std::string_view type = ts_node_type(ancestor);
    if (is_function_node(type)) {
      if (binds_parameter(ancestor, name, source)) {
        return std::nullopt;
      }
      continue;
    }
    if (type == "program") {
      return std::nullopt;
    }
    if (header_binds(ancestor, name, source)) {
      return LocalBinding{.scope = ancestor, .opaque = true};
    }
    if (type != "statement_block") {
      continue;
    }
    const auto count = ts_node_named_child_count(ancestor);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode statement = ts_node_named_child(ancestor, index);
      const std::string_view statement_type = ts_node_type(statement);
      if (statement_type != "lexical_declaration" && statement_type != "variable_declaration") {
        continue;
      }
      const auto declarators = ts_node_named_child_count(statement);
      for (std::uint32_t d = 0; d < declarators; ++d) {
        const TSNode declarator = ts_node_named_child(statement, d);
        if (std::string_view(ts_node_type(declarator)) == "variable_declarator" &&
            field_text(declarator, "name", source) == name) {
          const TSNode keyword = ts_node_child(statement, 0);
          return LocalBinding{
              .declarator = declarator,
              .scope = ancestor,
              .constant = !ts_node_is_null(keyword) && std::string_view(ts_node_type(keyword)) == "const",
          };
        }
      }
    }
  }
  return std::nullopt;
}

// The nearest function `node` sits in, or null at module level.
[[nodiscard]] TSNode enclosing_function(const TSNode& node) {
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    if (is_function_node(ts_node_type(ancestor))) {
      return ancestor;
    }
  }
  return TSNode{};
}

// The if/else branches `node` sits in below `stop`: (the `if`, 0 for its
// consequence or 1 for its alternative) for each enclosing `if`.
[[nodiscard]] std::vector<std::pair<const void*, int>> branch_path(TSNode node, const TSNode& stop) {
  std::vector<std::pair<const void*, int>> path;
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent) && !ts_node_eq(node, stop);
       node = parent, parent = ts_node_parent(parent)) {
    if (std::string_view(ts_node_type(parent)) != "if_statement") {
      continue;
    }
    if (ts_node_eq(node, ts_node_child_by_field_name(parent, "consequence", 11))) {
      path.emplace_back(parent.id, 0);
    } else if (ts_node_eq(node, ts_node_child_by_field_name(parent, "alternative", 11))) {
      path.emplace_back(parent.id, 1);
    }
  }
  return path;
}

// True when `a` and `b` sit in opposite branches of one `if`/`else`: at most
// one of them runs.
[[nodiscard]] bool exclusive_branches(const TSNode& a, const TSNode& b, const TSNode& stop) {
  const auto first = branch_path(a, stop);
  const auto second = branch_path(b, stop);
  for (const auto& [branch, side] : first) {
    for (const auto& [other, other_side] : second) {
      if (branch == other && side != other_side) {
        return true;
      }
    }
  }
  return false;
}

// True when a loop sits between `node` and `stop`: code in one branch of an
// if/else there can run before the other branch on a later pass.
[[nodiscard]] bool loops_between(const TSNode& node, const TSNode& stop) {
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor) && !ts_node_eq(ancestor, stop);
       ancestor = ts_node_parent(ancestor)) {
    const std::string_view type = ts_node_type(ancestor);
    if (type == "for_statement" || type == "for_in_statement" || type == "while_statement" || type == "do_statement") {
      return true;
    }
  }
  return false;
}

// True when `name` is read anywhere in `value` (`url = url + '/x'`).
[[nodiscard]] bool mentions(const TSNode& value, std::string_view name, std::string_view source) {
  if (ts_node_is_null(value)) {
    return false;
  }
  bool found = std::string_view(ts_node_type(value)) == "identifier" && node_text(value, source) == name;
  visit_named_descendants(value, false, [&](const TSNode& node) {
    found = found || (std::string_view(ts_node_type(node)) == "identifier" && node_text(node, source) == name);
  });
  return found;
}

// Every value the local `name` can hold where `use` reads it. Only a local
// set once is read: its initializer with no assignment after it, or, with no
// initializer, assignments before `use` in opposite branches of one if/else
// (`let url; if (a) url = x; else url = y;`). A write in the other branch of an
// if/else from the read never reaches it (`if (a) { url = x } else { url = y;
// fetch(url) }` reads only y) unless a loop around the read can carry it back
// round. Empty optional when `name` is not a local; an empty list when it is
// one whose value cannot be known here: it is reassigned (`url = url + '/x'`,
// a second `url = ...`, a loop's `next = ...`, a destructuring `({ url } =
// cfg)` or `[url] = cfg`), updated in place (`url +=`), assigned inside
// another function than the read (a test's `beforeAll`), bound by a loop
// header or `catch`, built from itself, its path is rewritten (`url.pathname =
// ...`), or it is never given a value.
[[nodiscard]] std::optional<std::vector<TSNode>> local_values(const TSNode& use, std::string_view name, std::string_view source) {
  const auto binding = local_binding(use, name, source);
  if (!binding) {
    return std::nullopt;
  }
  if (binding->opaque) {
    return std::vector<TSNode>{};
  }
  const TSNode initial = ts_node_child_by_field_name(binding->declarator, "value", 5);
  bool unknowable = false;
  std::vector<TSNode> assignments;
  const auto use_start = ts_node_start_byte(use);
  const TSNode use_function = enclosing_function(use);
  const bool use_in_loop = loops_between(use, binding->scope);
  visit_named_descendants(binding->scope, false, [&](const TSNode& node) {
    const std::string_view type = ts_node_type(node);
    if ((type == "update_expression" || type == "assignment_expression" || type == "augmented_assignment_expression") &&
        !use_in_loop && exclusive_branches(node, use, binding->scope)) {
      return;  // the other branch of an if/else from the read
    }
    if (type == "update_expression") {
      const TSNode argument = unwrap_expression(ts_node_child_by_field_name(node, "argument", 8));
      unknowable = unknowable || (!ts_node_is_null(argument) && node_text(argument, source) == name);
      return;
    }
    if (type != "assignment_expression" && type != "augmented_assignment_expression") {
      return;
    }
    const TSNode left = unwrap_expression(ts_node_child_by_field_name(node, "left", 4));
    if (ts_node_is_null(left)) {
      return;
    }
    const std::string_view left_type = ts_node_type(left);
    if (left_type == "member_expression") {
      const TSNode object = unwrap_expression(ts_node_child_by_field_name(left, "object", 6));
      const auto property = field_text(left, "property", source);
      if (!ts_node_is_null(object) && std::string_view(ts_node_type(object)) == "identifier" &&
          node_text(object, source) == name && (property == "pathname" || property == "href")) {
        unknowable = true;
      }
      return;
    }
    if (left_type == "object_pattern" || left_type == "array_pattern") {
      if (binds_pattern(left, name, source)) {
        const auto same = local_binding(left, name, source);
        unknowable = unknowable || (same && !same->opaque && ts_node_eq(same->declarator, binding->declarator));
      }
      return;
    }
    if (left_type != "identifier" || node_text(left, source) != name) {
      return;
    }
    const auto same = local_binding(left, name, source);
    if (!same || same->opaque || !ts_node_eq(same->declarator, binding->declarator)) {
      return;  // a nested declaration of the same name
    }
    if (type == "augmented_assignment_expression" || !ts_node_eq(enclosing_function(node), use_function) ||
        ts_node_start_byte(node) >= use_start) {
      unknowable = true;  // updated in place, set by a callback that runs who knows when, or after the read
      return;
    }
    assignments.push_back(node);
  });
  std::vector<TSNode> values;
  if (!ts_node_is_null(initial)) {
    unknowable = unknowable || !assignments.empty();  // reassigned: which value the read sees is a guess
    values.push_back(unwrap_expression(initial));
  }
  for (std::size_t i = 0; i < assignments.size() && !unknowable; ++i) {
    for (std::size_t j = i + 1; j < assignments.size() && !unknowable; ++j) {
      unknowable = !exclusive_branches(assignments[i], assignments[j], binding->scope);
    }
    values.push_back(unwrap_expression(ts_node_child_by_field_name(assignments[i], "right", 5)));
  }
  for (const auto& value : values) {
    unknowable = unknowable || mentions(value, name, source);
  }
  if (unknowable) {
    values.clear();
  }
  return values;
}

// The values a function returns, not counting nested functions: an arrow's
// expression body, else every `return <expr>`. A bare `return;` yields null.
void function_return_values(const TSNode& function, std::vector<TSNode>& out) {
  const TSNode body = ts_node_child_by_field_name(function, "body", 4);
  if (ts_node_is_null(body)) {
    return;
  }
  if (std::string_view(ts_node_type(body)) != "statement_block") {
    out.push_back(unwrap_expression(body));
    return;
  }
  visit_named_descendants(body, true, [&](const TSNode& node) {
    if (std::string_view(ts_node_type(node)) == "return_statement") {
      out.push_back(ts_node_named_child_count(node) == 0 ? TSNode{} : unwrap_expression(ts_node_named_child(node, 0)));
    }
  });
}

// True for a value that is a query string or nothing: `''`, `'?a=1'`,
// `\`?${qs}\``, or a conditional choosing between such values.
[[nodiscard]] bool is_query_value(const TSNode& value, std::string_view source) {
  if (ts_node_is_null(value)) {
    return false;
  }
  const std::string_view type = ts_node_type(value);
  if (type == "ternary_expression") {
    return is_query_value(unwrap_expression(ts_node_child_by_field_name(value, "consequence", 11)), source) &&
           is_query_value(unwrap_expression(ts_node_child_by_field_name(value, "alternative", 11)), source);
  }
  if (type == "string") {
    const auto text = strip_string_quotes(node_text(value, source));
    return text.empty() || text.front() == '?';
  }
  if (type == "template_string") {
    if (ts_node_named_child_count(value) == 0) {
      return strip_string_quotes(node_text(value, source)).empty();
    }
    const TSNode first = ts_node_named_child(value, 0);
    return std::string_view(ts_node_type(first)) == "string_fragment" && node_text(first, source).starts_with("?");
  }
  return false;
}

// A function returning only query strings (`buildQuery(q)` returning `''` or
// `?a=1`).
[[nodiscard]] bool is_query_helper(const TSNode& function, std::string_view source) {
  std::vector<TSNode> returns;
  function_return_values(function, returns);
  return !returns.empty() && std::ranges::all_of(returns, [&](const TSNode& value) { return is_query_value(value, source); });
}

// A value that starts the query string: a query literal, or a call to a query
// helper this file defines (`const qs = buildQuery(q); \`/score${qs}\``).
[[nodiscard]] bool starts_query(const TSNode& value, std::string_view source) {
  if (is_query_value(value, source)) {
    return true;
  }
  if (ts_node_is_null(value) || std::string_view(ts_node_type(value)) != "call_expression") {
    return false;
  }
  const TSNode callee = unwrap_expression(ts_node_child_by_field_name(value, "function", 8));
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "identifier") {
    return false;
  }
  const TSNode function = module_function(value, node_text(callee, source), source);
  return !ts_node_is_null(function) && is_query_helper(function, source);
}

[[nodiscard]] int parameter_index(const std::vector<std::string>& parameters, std::string_view name) {
  const auto found = std::ranges::find(parameters, name);
  return found == parameters.end() ? -1 : static_cast<int>(found - parameters.begin());
}

// The parameters of the function a node sits in (empty at module level).
[[nodiscard]] std::vector<std::string> enclosing_parameters(const TSNode& node, std::string_view source) {
  std::vector<std::string> parameters;
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    if (is_function_node(ts_node_type(ancestor))) {
      parameter_names(ancestor, source, parameters);
      break;
    }
  }
  return parameters;
}

// The names a URL template may read as parameters, and whether a parameter at
// the end is a tail its callers supply (a wrapper) or just a value (a URL
// builder's argument, which fills a segment at most).
struct UrlScope {
  std::vector<std::string> parameters;
  bool wrapper = true;
};

// A URL argument reduced to the path it names. Literal text is kept; an
// interpolation at the start is the host and is dropped, unless it is a call
// that builds the URL from a runtime value (then the URL is unresolvable); one that fills a whole
// segment is a parameter, `{}`; a parameter of the enclosing function at the
// end is the tail a wrapper appends its argument to (and one directly before
// that tail is a base its callers supply; one before literal text is a name
// like any other at the host); anything else mid-segment
// makes the URL unresolvable. The query string and fragment are not part of the
// route. A literal absolute URL names another service and is unresolvable here.
struct UrlTemplate {
  std::string path;
  bool tail = false;
  int tail_index = -1;  // the enclosing function's parameter that is the tail
  int base_index = -1;  // the enclosing function's parameter that is the base
  std::string tail_name;  // the tail parameter's name, read as a name if text follows it
  bool resolvable = true;
  bool in_query = false;
  bool dropped_host = false;
  bool relative = false;  // `v1/users`: a path its client joins to a base URL

  void literal(std::string_view text) {
    if (in_query) {
      return;
    }
    if (tail && !text.empty()) {
      if (!path.empty() || base_index >= 0) {
        resolvable = false;  // text after the appended argument: not a prefix wrapper
        return;
      }
      // `${apiUrl}/api/...` with `apiUrl` a parameter: not a tail but the host,
      // a name read like any other there (a `${apiUrl}` reference).
      path = "${" + tail_name + "}";
      tail_name.clear();
      tail_index = -1;
      tail = false;
    }
    const auto cut = text.find_first_of("?#");
    path.append(text.substr(0, cut));
    if (cut != std::string_view::npos) {
      in_query = true;
    }
  }
  // An interpolation whose value this file cannot read. `opaque` says whether it
  // could hold a path: an in-file constant built from `process.env` is a host
  // and nothing more, while an imported `API_BASE` or a `config.baseUrl` member
  // may well end in `/api/v1`.
  void unknown(bool opaque) {
    if (in_query) {
      return;
    }
    if (tail) {
      resolvable = false;  // `${path}${x}`: nothing may follow the tail
      return;
    }
    if (path.empty()) {
      dropped_host = dropped_host || opaque;  // the host: `${API_URL}/api/v1/...`
      return;
    }
    if (path.back() == '/') {
      path += "{}";  // a whole-segment parameter: `/notes/${id}/star`
      return;
    }
    resolvable = false;  // `/v1-${x}`: a partial segment no router template matches
  }
  // A call that builds the URL from a runtime value (`${base(projectId)}/oracles`)
  // with a builder this file does not define. A local, member or host getter in
  // front is the host (`${apiUrl}/api/v1/...`, `${getAgentsApiUrl()}/runs/wait`),
  // but a builder's result may end in a path this file cannot see; dropping it
  // as the host would mint a truncated route that matches the wrong provider or
  // none. Leave it unresolved.
  void built_by_call() {
    if (in_query) {
      return;
    }
    if (path.empty()) {
      resolvable = false;
      return;
    }
    unknown(true);
  }
  // The query string starts here: a same-file helper that returns `''` or
  // `?a=1` (`/formulations/score${buildQuery(q)}`).
  void query() {
    in_query = true;
  }
  // The enclosing function's parameter `index` interpolated into the URL. After
  // a slash it fills a segment like any other value (`/projects/${projectId}/publish`
  // in `publishProject(projectId)`); appended to text (`${base}${path}`) or
  // standing alone (`fetch(url)`) it is the tail a wrapper forwards, and a
  // parameter right before that tail (`${baseUrl}${path}`) is the base. A URL
  // builder's parameter is a value, never a tail: it only fills a segment.
  void parameter(int index, std::string_view name, bool wrapper) {
    if (in_query) {
      return;
    }
    if (!path.empty() && path.back() == '/') {
      path += "{}";
      return;
    }
    if (!wrapper) {
      resolvable = false;  // a builder's argument as host or mid-segment: unknowable
      return;
    }
    if (tail) {
      if (path.empty() && base_index < 0) {
        base_index = tail_index;
        tail_index = index;
        tail_name = name;
        return;
      }
      resolvable = false;
      return;
    }
    if (path.empty() && dropped_host) {
      // `${API_BASE}${path}` with a base this file does not define: the base may
      // hold a path (`/api/v1`) we cannot see, so the prefix is unknowable. An
      // in-file `process.env` host before the tail is fine: the prefix is empty.
      resolvable = false;
      return;
    }
    tail = true;
    tail_index = index;
    tail_name = name;
  }
  // `${API_BASE}/${endpoint.replace(/^\//, '')}`: the parameter with its
  // leading slash stripped is a path joined after the slash, so it is the
  // tail even though a slash precedes it.
  void stripped_parameter(int index) {
    if (in_query) {
      return;
    }
    if (tail || path.empty() || path.back() != '/') {
      resolvable = false;
      return;
    }
    tail = true;
    tail_index = index;
  }
  // An identifier this file does not define, in the base position: kept as a
  // `${NAME}` placeholder for resolve_contracts, which inlines the constant when
  // exactly one file in the project defines a URL constant of that name.
  void reference(std::string_view name) {
    if (in_query) {
      return;
    }
    if (!path.empty() || tail) {
      unknown(true);
      return;
    }
    path = "${" + std::string(name) + "}";
  }
  // `allow_relative`: a path without a leading slash is kept (and marked) when
  // its client joins it to a base URL (an axios `baseURL`, a wrapper's prefix).
  void finish(bool allow_relative = false) {
    if (path.starts_with("http://") || path.starts_with("https://")) {
      resolvable = false;  // another host, spelled out: not this repository's contract
      return;
    }
    if (!tail && (path.empty() || (path.front() != '/' && !path.starts_with("${")))) {
      if (allow_relative && !path.empty() && std::isalnum(static_cast<unsigned char>(path.front())) != 0 &&
          path.find('/') != std::string::npos && path.find_first_of(": ") == std::string::npos) {
        relative = true;
        return;
      }
      resolvable = false;
    }
  }
};

constexpr int kMaxUrlInlineDepth = 3;

// A value read on its own that names a path: resolvable, not a bare host, not
// another service's absolute URL.
[[nodiscard]] bool reads_as_path(const UrlTemplate& whole) {
  return whole.resolvable && (whole.tail || !whole.path.empty()) && !whole.path.starts_with("http://") &&
         !whole.path.starts_with("https://");
}

// A call at the front of a URL either returns a host (`getAgentsApiUrl()`,
// `config.get('apiUrl')`, `process.env.API_URL?.replace(/\/$/, '')`) or builds
// part of the path from a runtime value (`base(projectId)`). Only the second
// hides path segments; the first is read as the host like a constant would be.
[[nodiscard]] bool builds_url_from_values(const TSNode& call, std::string_view source) {
  if (node_text(call, source).starts_with("process.env.")) {
    return false;
  }
  const TSNode arguments = ts_node_child_by_field_name(call, "arguments", 9);
  if (ts_node_is_null(arguments)) {
    return false;
  }
  for (uint32_t i = 0; i < ts_node_named_child_count(arguments); ++i) {
    const TSNode argument = ts_node_named_child(arguments, i);
    const std::string_view argument_type = ts_node_type(argument);
    if (argument_type == "string" || argument_type == "comment") {
      continue;
    }
    // A template with no substitution (`getUrl(\`api\`)`) is a constant too.
    bool substituted = argument_type != "template_string";
    for (uint32_t j = 0; !substituted && j < ts_node_named_child_count(argument); ++j) {
      substituted = std::string_view(ts_node_type(ts_node_named_child(argument, j))) == "template_substitution";
    }
    if (substituted) {
      return true;
    }
  }
  return false;
}

// `path.replace(/^\//, '')` (or `/^\/+/`): strips a leading slash, nothing else.
[[nodiscard]] bool strips_leading_slash(const TSNode& arguments, std::string_view source) {
  if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) != 2) {
    return false;
  }
  const TSNode pattern = ts_node_named_child(arguments, 0);
  const TSNode replacement = ts_node_named_child(arguments, 1);
  if (std::string_view(ts_node_type(pattern)) != "regex" || !is_string_value(replacement) ||
      !strip_string_quotes(node_text(replacement, source)).empty()) {
    return false;
  }
  const auto text = node_text(pattern, source);
  return text == "/^\\//" || text == "/^\\/+/";
}

// `X || '/api/roles'`, `X ?? '/api'`: a value whose fallback is a path, so
// the value is a path too, not a host.
[[nodiscard]] bool has_path_fallback(const TSNode& value, std::string_view source) {
  if (std::string_view(ts_node_type(value)) != "binary_expression") {
    return false;
  }
  const auto op = field_text(value, "operator", source);
  const TSNode right = unwrap_expression(ts_node_child_by_field_name(value, "right", 5));
  return (op == "||" || op == "??") && !ts_node_is_null(right) && is_string_value(right) &&
         strip_string_quotes(node_text(right, source)).starts_with("/");
}

void collect_url_template(const TSNode& node, const ExtractionContext& context, const UrlScope& scope,
                          UrlTemplate& url, int depth) {
  const TSNode expression = unwrap_expression(node);
  if (ts_node_is_null(expression)) {
    url.resolvable = false;
    return;
  }
  const std::string_view type = ts_node_type(expression);
  if (type == "string") {
    url.literal(strip_string_quotes(node_text(expression, context.source)));
    return;
  }
  if (type == "template_string") {
    const auto count = ts_node_named_child_count(expression);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode part = ts_node_named_child(expression, index);
      const std::string_view part_type = ts_node_type(part);
      if (part_type == "string_fragment") {
        url.literal(node_text(part, context.source));
      } else if (part_type == "template_substitution") {
        if (ts_node_named_child_count(part) == 0) {
          url.unknown(true);
          continue;
        }
        collect_url_template(ts_node_named_child(part, 0), context, scope, url, depth);
      }
    }
    return;
  }
  if (type == "binary_expression") {
    const TSNode op = ts_node_child_by_field_name(expression, "operator", 8);
    if (ts_node_is_null(op)) {
      url.unknown(true);
      return;
    }
    const auto op_text = node_text(op, context.source);
    if (op_text == "||" || op_text == "??") {
      // `process.env.API_URL || 'http://localhost:8080'`: a host with a fallback.
      url.unknown(false);
      return;
    }
    if (op_text != "+") {
      url.unknown(true);
      return;
    }
    // `API + '/notebooks'`: string concatenation reads like a template.
    collect_url_template(ts_node_child_by_field_name(expression, "left", 4), context, scope, url, depth);
    collect_url_template(ts_node_child_by_field_name(expression, "right", 5), context, scope, url, depth);
    return;
  }
  if (type == "identifier") {
    const auto name = node_text(expression, context.source);
    if (const int index = parameter_index(scope.parameters, name); index >= 0) {
      url.parameter(index, name, scope.wrapper);
      return;
    }
    // A variable declared in an enclosing block is local: no constant anywhere
    // can stand for it. Standing for the whole URL (`const url = \`${BASE}/x\`;
    // fetch(url)`) it is read through its one value when that value reads as a
    // path; any other local there (`const apiUrl = 'http://localhost:8080'`, a
    // call, several values) is the host, and one inside the path is a value
    // filling a segment, whatever this run happened to assign it, unless it
    // holds the query string.
    if (const auto values = local_values(expression, name, context.source)) {
      if (values->size() == 1 && !url.path.empty() && starts_query(values->front(), context.source)) {
        url.query();
        return;
      }
      if (values->size() == 1 && depth < kMaxUrlInlineDepth && url.path.empty() && !url.tail && !url.dropped_host &&
          !url.in_query) {
        UrlTemplate whole;
        collect_url_template(values->front(), context, scope, whole, depth + 1);
        if (reads_as_path(whole)) {
          url = std::move(whole);
          return;
        }
      }
      url.unknown(true);
      return;
    }
    if (depth < kMaxUrlInlineDepth) {
      if (const TSNode value = module_const_value(expression, name, context.source); !ts_node_is_null(value)) {
        const std::string_view value_type = ts_node_type(value);
        if (value_type == "string" || value_type == "template_string" || value_type == "binary_expression") {
          collect_url_template(value, context, {}, url, depth + 1);
          return;
        }
        // Defined here from something that is not a string (`process.env.X`, a
        // call): whatever it is, it is the host.
        url.unknown(false);
        return;
      }
    }
    url.reference(name);  // imported or otherwise unknown: resolved project-wide, or refused
    return;
  }
  if (type == "member_expression") {
    if (node_text(expression, context.source).starts_with("process.env.")) {
      url.unknown(false);
      return;
    }
    const auto property = field_text(expression, "property", context.source);
    const TSNode object = unwrap_expression(ts_node_child_by_field_name(expression, "object", 6));
    if (property == "href" && !ts_node_is_null(object)) {
      collect_url_template(object, context, scope, url, depth);  // `url.href`: the URL itself
      return;
    }
    // `${this.API_BASE}/v1/...`: a string field of this class, set once, read
    // when its value reads as a path. A field that is some path we cannot read
    // (`API_ENDPOINTS.roles || '/api/roles'`) is not a host to drop: the URL
    // is unresolvable. Anything else there is the host, as before.
    if (!ts_node_is_null(object) && std::string_view(ts_node_type(object)) == "this" && depth < kMaxUrlInlineDepth) {
      if (const TSNode body = enclosing_class_body(expression); !ts_node_is_null(body)) {
        if (const TSNode value = class_member_value(body, property, context.source);
            !ts_node_is_null(value) && (is_string_value(value) || std::string_view(ts_node_type(value)) == "binary_expression")) {
          UrlTemplate whole;
          collect_url_template(value, context, {}, whole, depth + 1);
          if (reads_as_path(whole) && !whole.tail) {
            collect_url_template(value, context, {}, url, depth + 1);
            return;
          }
          if (has_path_fallback(value, context.source)) {
            url.resolvable = false;
            return;
          }
        }
      }
    }
    url.unknown(true);
    return;
  }
  if (type == "new_expression") {
    // `new URL(\`${BASE}/x\`)` is that URL; `new URL(path, base)` resolves
    // `path` against a base this reading does not follow.
    if (field_text(expression, "constructor", context.source) == "URL") {
      const TSNode arguments = ts_node_child_by_field_name(expression, "arguments", 9);
      if (!ts_node_is_null(arguments) && ts_node_named_child_count(arguments) == 1) {
        collect_url_template(ts_node_named_child(arguments, 0), context, scope, url, depth);
        return;
      }
      url.resolvable = false;
      return;
    }
  }
  if (type == "call_expression") {
    const TSNode callee = unwrap_expression(ts_node_child_by_field_name(expression, "function", 8));
    const TSNode arguments = ts_node_child_by_field_name(expression, "arguments", 9);
    const std::string_view callee_type = ts_node_is_null(callee) ? std::string_view{} : ts_node_type(callee);
    if (callee_type == "member_expression") {
      const auto method = field_text(callee, "property", context.source);
      const TSNode object = unwrap_expression(ts_node_child_by_field_name(callee, "object", 6));
      if (method == "toString" && !ts_node_is_null(arguments) && ts_node_named_child_count(arguments) == 0 &&
          !ts_node_is_null(object)) {
        collect_url_template(object, context, scope, url, depth);  // `url.toString()`: the URL itself
        return;
      }
      if (method == "replace" && scope.wrapper && !ts_node_is_null(object) &&
          std::string_view(ts_node_type(object)) == "identifier" && strips_leading_slash(arguments, context.source)) {
        if (const int index = parameter_index(scope.parameters, node_text(object, context.source)); index >= 0) {
          url.stripped_parameter(index);
          return;
        }
      }
    }
    // A URL builder or query helper this file defines is read, not guessed:
    // `const base = (projectId) => \`${API}/projects/${projectId}\`` inlines
    // with its parameters as segment values; a helper returning only `''` or
    // `?...` starts the query string.
    if (callee_type == "identifier" && depth < kMaxUrlInlineDepth) {
      if (const TSNode function = module_function(expression, node_text(callee, context.source), context.source);
          !ts_node_is_null(function)) {
        if (is_query_helper(function, context.source)) {
          url.query();
          return;
        }
        std::vector<TSNode> returns;
        function_return_values(function, returns);
        if (returns.size() == 1 && !ts_node_is_null(returns.front())) {
          UrlScope builder{.wrapper = false};
          parameter_names(function, context.source, builder.parameters);
          UrlTemplate whole;
          collect_url_template(returns.front(), context, builder, whole, depth + 1);
          if (reads_as_path(whole)) {
            collect_url_template(returns.front(), context, builder, url, depth + 1);
            return;
          }
          // A helper returning a host (`getAgentsApiUrl()`) is read like any
          // other call there, below.
        }
      }
    }
    if (builds_url_from_values(expression, context.source)) {
      url.built_by_call();
      return;
    }
  }
  url.unknown(true);
}

// The receiver of `X.get(url)` is an HTTP client when its name says so. Without
// this, `map.get('/key')` and `router.get('/path', handler)` would read as
// requests; the route shape is excluded separately by its handler argument.
[[nodiscard]] bool looks_like_http_client(std::string_view receiver) {
  std::string lower(receiver);
  for (auto& ch : lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  for (const auto* hint : {"api", "client", "axios", "ky", "got", "http", "fetch", "request", "agent"}) {
    if (lower.find(hint) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// What a consumer call is attributed to: the enclosing function when there is
// one, else the module-level variable the call helps initialise (`export const
// notebooksApi = { list: () => apiFetch('/notebooks') }`: the arrow is a
// boundary, the object is the symbol), else the file.
[[nodiscard]] std::string consumer_scope_id(const TSNode& node, const ExtractionContext& context,
                                            const std::string& function_scope_id) {
  if (!function_scope_id.empty()) {
    return function_scope_id;
  }
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    if (std::string_view(ts_node_type(ancestor)) != "variable_declarator") {
      continue;
    }
    const TSNode declaration = ts_node_parent(ancestor);
    if (!ts_node_is_null(declaration) && is_module_level_declaration(declaration)) {
      return make_id(context.relative_path + ":" + field_text(ancestor, "name", context.source));
    }
  }
  return make_id(context.relative_path);
}

[[nodiscard]] std::string upper_verb(std::string text) {
  for (auto& ch : text) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return text;
}

// The HTTP verb a string literal spells (`'POST'`, `"get"`), uppercased; empty
// for anything else.
[[nodiscard]] std::string literal_verb(const TSNode& node, std::string_view source) {
  const TSNode value = unwrap_expression(node);
  if (ts_node_is_null(value) || std::string_view(ts_node_type(value)) != "string") {
    return {};
  }
  auto text = strip_string_quotes(node_text(value, source));
  std::string lower(text);
  for (auto& ch : lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return is_http_verb(lower) && lower != "all" ? upper_verb(std::move(text)) : std::string{};
}

// Where a request's method comes from: a fixed verb, a parameter of the
// enclosing function (`fetch(url, { method })` in `request(method, path)`), or
// an options parameter spread in after any fixed verb (`{ method: 'GET',
// ...init }`: `init`'s method, when it has one, wins; `{ ...options, method:
// 'PATCH' }` is PATCH whatever `options` holds). `unknown` when the options
// may carry a method this file cannot read: the object is a value it cannot
// see, or something other than a parameter is spread in after any fixed verb.
struct MethodSource {
  std::string verb;
  int parameter = -1;
  int options = -1;  // the enclosing function's parameter whose own `method` overrides `verb`
  std::vector<std::string> choices;  // `method: on ? 'POST' : 'DELETE'`: each verb is sent
  bool unknown = false;

  bool operator==(const MethodSource&) const = default;
};

constexpr int kMaxOptionsDepth = 3;

// The parameter a local takes the rest of: `const { skipRetry, ...rest } =
// options` makes `rest` the parameter `options` less the named keys, method
// included unless it is named. -1 for anything else.
[[nodiscard]] int rest_of_parameter(const TSNode& use, std::string_view name, const std::vector<std::string>& parameters,
                                    std::string_view source) {
  for (TSNode ancestor = ts_node_parent(use); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    const std::string_view type = ts_node_type(ancestor);
    if (is_function_node(type) || type == "program") {
      return -1;
    }
    if (type != "statement_block") {
      continue;
    }
    const auto count = ts_node_named_child_count(ancestor);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode statement = ts_node_named_child(ancestor, index);
      const std::string_view statement_type = ts_node_type(statement);
      if (statement_type != "lexical_declaration" && statement_type != "variable_declaration") {
        continue;
      }
      const auto declarators = ts_node_named_child_count(statement);
      for (std::uint32_t d = 0; d < declarators; ++d) {
        const TSNode declarator = ts_node_named_child(statement, d);
        const TSNode pattern = ts_node_child_by_field_name(declarator, "name", 4);
        if (ts_node_is_null(pattern) || std::string_view(ts_node_type(pattern)) != "object_pattern") {
          continue;
        }
        bool rest = false;
        bool names_method = false;
        const auto members = ts_node_named_child_count(pattern);
        for (std::uint32_t m = 0; m < members; ++m) {
          const TSNode member = ts_node_named_child(pattern, m);
          const std::string_view member_type = ts_node_type(member);
          if (member_type == "rest_pattern") {
            rest = rest || binds_pattern(member, name, source);
          } else if (member_type == "shorthand_property_identifier_pattern") {
            names_method = names_method || node_text(member, source) == "method";
          } else if (member_type == "pair_pattern") {
            names_method = names_method || strip_string_quotes(field_text(member, "key", source)) == "method";
          } else if (member_type == "object_assignment_pattern") {
            names_method = names_method || field_text(member, "left", source) == "method";
          }
        }
        if (!rest) {
          continue;
        }
        const TSNode value = unwrap_expression(ts_node_child_by_field_name(declarator, "value", 5));
        if (names_method || ts_node_is_null(value) || std::string_view(ts_node_type(value)) != "identifier") {
          return -1;
        }
        return parameter_index(parameters, node_text(value, source));
      }
    }
  }
  return -1;
}

// The `method` of an options object, members read in order so a later one
// wins: `method: 'POST'` is fixed, `method` / `method: m` naming a parameter
// of the enclosing function is that parameter, `...init` spreading a parameter
// lets that parameter's method override what came before, `...{ method: 'HEAD'
// }` or a spread local is read as that object. An options parameter passed as
// the whole object (`fetch(url, init)`) is that parameter; a local set once
// (`const opts = { ...init, method: 'DELETE' }`) is read through its value, and
// `rest` from `const { a, ...rest } = options` is the parameter `options`.
// Absent is empty; any other value, method or spread the file cannot read is
// `unknown`.
[[nodiscard]] MethodSource options_method(const TSNode& options, const std::vector<std::string>& parameters,
                                          std::string_view source, int depth = 0) {
  const TSNode object = unwrap_expression(options);
  if (ts_node_is_null(object)) {
    return {};
  }
  const std::string_view type = ts_node_type(object);
  if (type == "identifier") {
    const auto name = node_text(object, source);
    if (const int at = parameter_index(parameters, name); at >= 0) {
      return MethodSource{.options = at};
    }
    if (depth < kMaxOptionsDepth) {
      if (const auto values = local_values(object, name, source)) {
        return values->size() == 1 ? options_method(values->front(), parameters, source, depth + 1)
                                   : MethodSource{.unknown = true};
      }
      if (const int at = rest_of_parameter(object, name, parameters, source); at >= 0) {
        return MethodSource{.options = at};
      }
    }
    return MethodSource{.unknown = true};
  }
  if (type != "object") {
    return MethodSource{.unknown = true};
  }
  MethodSource found;
  const auto count = ts_node_named_child_count(object);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(object, index);
    const std::string_view member_type = ts_node_type(member);
    if (member_type == "spread_element") {
      const TSNode spread = ts_node_named_child_count(member) == 0 ? TSNode{} : ts_node_named_child(member, 0);
      const auto inner = depth < kMaxOptionsDepth ? options_method(spread, parameters, source, depth + 1)
                                                  : MethodSource{.unknown = true};
      if (!inner.verb.empty() || inner.parameter >= 0 || !inner.choices.empty()) {
        found = inner;  // a method of its own replaces what came before
      } else {
        if (inner.options >= 0) {
          found.options = inner.options;  // its method, if any, overrides what came before
        }
        found.unknown = found.unknown || inner.unknown;
      }
      continue;
    }
    if (member_type == "shorthand_property_identifier" && node_text(member, source) == "method") {
      const int at = parameter_index(parameters, "method");
      found = at >= 0 ? MethodSource{.parameter = at} : MethodSource{.unknown = true};
      continue;
    }
    if (member_type != "pair" || strip_string_quotes(field_text(member, "key", source)) != "method") {
      continue;
    }
    const TSNode value = unwrap_expression(ts_node_child_by_field_name(member, "value", 5));
    found = MethodSource{.unknown = true};
    if (ts_node_is_null(value)) {
      continue;
    }
    if (is_string_value(value)) {
      found = MethodSource{.verb = upper_verb(strip_string_quotes(node_text(value, source)))};
    } else if (std::string_view(ts_node_type(value)) == "identifier") {
      if (const int at = parameter_index(parameters, node_text(value, source)); at >= 0) {
        found = MethodSource{.parameter = at};
      }
    } else if (std::string_view(ts_node_type(value)) == "ternary_expression") {
      auto consequence = literal_verb(ts_node_child_by_field_name(value, "consequence", 11), source);
      auto alternative = literal_verb(ts_node_child_by_field_name(value, "alternative", 11), source);
      if (!consequence.empty() && !alternative.empty()) {
        found = MethodSource{.choices = {std::move(consequence), std::move(alternative)}};
      }
    }
  }
  return found;
}

// One HTTP fact a call yields, before it becomes a RawRelation.
struct ClientCall {
  enum class Kind { Consumer, Wrapper, Arguments };
  Kind kind = Kind::Consumer;
  std::string client;
  MethodSource method;
  std::string path;  // Consumer: the path (empty when unresolvable); Wrapper: the prefix; Arguments: descriptors
  int tail = 0;      // Wrapper: the parameter its callers' path fills
  int base = -1;     // Wrapper: the parameter its callers' base URL fills (composed in-file only)

  [[nodiscard]] bool same_shape(const ClientCall& other) const {
    return method == other.method && path == other.path && tail == other.tail && base == other.base;
  }
};

constexpr int kMaxWrapperDepth = 3;

std::vector<ClientCall> analyze_client_call(const TSNode& node, const ExtractionContext& context, int depth);

// A function of this file as a wrapper: the one shape every forwarding client
// call in its body agrees on (`patch(url) { return this.api.patch(url) }` is a
// PATCH wrapper under the instance's baseURL). Empty when no call forwards a
// parameter as the path, or two calls disagree.
// Wrapper shapes already worked out for the file being extracted on this
// thread, by function node and depth (a shape found deeper sees fewer levels).
// Without it every `this.x()` call re-reads x and everything x calls.
struct ShapeKey {
  const void* node;
  int depth;
  bool operator==(const ShapeKey&) const = default;
};
struct ShapeKeyHash {
  std::size_t operator()(const ShapeKey& key) const {
    return std::hash<const void*>{}(key.node) ^ (static_cast<std::size_t>(key.depth) << 1);
  }
};
using ShapeCache = std::unordered_map<ShapeKey, std::optional<ClientCall>, ShapeKeyHash>;
thread_local ShapeCache* active_shapes = nullptr;

[[nodiscard]] std::optional<ClientCall> read_wrapper_shape(const TSNode& function, const ExtractionContext& context, int depth);

[[nodiscard]] std::optional<ClientCall> wrapper_shape(const TSNode& function, const ExtractionContext& context, int depth) {
  if (depth > kMaxWrapperDepth) {
    return std::nullopt;
  }
  if (active_shapes == nullptr) {
    return read_wrapper_shape(function, context, depth);
  }
  const ShapeKey key{.node = function.id, .depth = depth};
  if (const auto found = active_shapes->find(key); found != active_shapes->end()) {
    return found->second;
  }
  auto shape = read_wrapper_shape(function, context, depth);
  active_shapes->insert_or_assign(key, shape);
  return shape;
}

[[nodiscard]] std::optional<ClientCall> read_wrapper_shape(const TSNode& function, const ExtractionContext& context, int depth) {
  const TSNode body = ts_node_child_by_field_name(function, "body", 4);
  if (ts_node_is_null(body)) {
    return std::nullopt;
  }
  std::optional<ClientCall> shape;
  bool agreed = true;
  const auto consider = [&](const TSNode& call) {
    if (!agreed || std::string_view(ts_node_type(call)) != "call_expression") {
      return;
    }
    for (auto& outcome : analyze_client_call(call, context, depth)) {
      if (outcome.kind != ClientCall::Kind::Wrapper) {
        continue;
      }
      if (!shape) {
        shape = std::move(outcome);
      } else if (!shape->same_shape(outcome)) {
        agreed = false;
      }
    }
  };
  consider(body);
  visit_named_descendants(body, true, consider);
  return agreed ? shape : std::nullopt;
}

// Joins a prefix and a path the way a client does: `/api/backend/` + `v1/x`
// or `/v1/x`; a relative path needs a prefix ending in a slash, and a `${X}`
// placeholder only stands at the front. Empty optional when the join is not
// knowable.
[[nodiscard]] std::optional<std::string> join_client_path(const std::string& prefix, const std::string& path) {
  if (path.empty()) {
    return prefix;
  }
  if (prefix.empty()) {
    return path;
  }
  if (path.starts_with("${")) {
    return std::nullopt;
  }
  if (path.front() != '/') {
    return prefix.back() == '/' ? std::optional<std::string>{prefix + path} : std::nullopt;
  }
  return prefix.back() == '/' ? prefix + path.substr(1) : prefix + path;
}

// A call into a wrapper this file defines, read through it: its callers' base,
// method and path arguments fill the wrapper's slots. A path argument that is
// itself the enclosing function's tail makes the enclosing function a wrapper
// too (`mlBackendRequest(method, path)` forwarding into `mlRequest(base,
// method, path)`); a complete path makes a consumer with the method known.
void compose_wrapper_call(const ClientCall& shape, std::string client, const TSNode& arguments,
                          const UrlScope& scope, const ExtractionContext& context, std::vector<ClientCall>& out) {
  const auto count = static_cast<int>(ts_node_named_child_count(arguments));
  const auto unresolved = [&] {
    out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = client, .method = {.verb = "GET"}});
  };
  if (shape.tail >= count) {
    unresolved();
    return;
  }
  std::string prefix = shape.path;
  if (shape.base >= 0) {
    if (shape.base >= count) {
      unresolved();
      return;
    }
    UrlTemplate base;
    collect_url_template(ts_node_named_child(arguments, static_cast<std::uint32_t>(shape.base)), context, scope, base, 0);
    if (!base.resolvable || base.tail || base.base_index >= 0 || base.dropped_host || base.in_query ||
        base.path.starts_with("http://") || base.path.starts_with("https://")) {
      unresolved();
      return;
    }
    prefix = base.path + prefix;
  }
  MethodSource method{.verb = shape.method.verb, .choices = shape.method.choices, .unknown = shape.method.unknown};
  // The call's own options override the wrapper's verb where the wrapper lets
  // them: at the options parameter it spreads in last, or, when the wrapper
  // fixes no verb, right after the path. Options whose method this file cannot
  // read leave the method unknown: the call is unresolved, not a guess.
  const int options_at = shape.method.options >= 0 ? shape.method.options
                         : shape.method.verb.empty() && shape.method.parameter < 0 && shape.method.choices.empty() &&
                                   !shape.method.unknown
                             ? shape.tail + 1
                             : -1;
  if (options_at >= 0 && options_at < count) {
    const auto given = options_method(ts_node_named_child(arguments, static_cast<std::uint32_t>(options_at)),
                                      scope.parameters, context.source);
    if (given.unknown) {
      method = MethodSource{.unknown = true};
    } else if (!given.verb.empty() || !given.choices.empty()) {
      method = MethodSource{.verb = given.verb, .choices = given.choices};
    } else if (given.parameter >= 0) {
      method = MethodSource{.parameter = given.parameter};
    }
    if (given.options >= 0) {
      method.options = given.options;  // the enclosing function's own options pass through
    }
  }
  if (shape.method.parameter >= 0) {
    if (shape.method.parameter >= count) {
      unresolved();
      return;
    }
    const TSNode argument = unwrap_expression(ts_node_named_child(arguments, static_cast<std::uint32_t>(shape.method.parameter)));
    if (auto verb = literal_verb(argument, context.source); !verb.empty()) {
      method.verb = std::move(verb);
    } else if (std::string_view(ts_node_type(argument)) == "identifier" &&
               parameter_index(scope.parameters, node_text(argument, context.source)) >= 0) {
      method.parameter = parameter_index(scope.parameters, node_text(argument, context.source));
    } else {
      unresolved();
      return;
    }
  }
  UrlTemplate url;
  collect_url_template(ts_node_named_child(arguments, static_cast<std::uint32_t>(shape.tail)), context, scope, url, 0);
  url.finish(true);
  if (url.tail) {
    if (!url.resolvable || url.base_index >= 0) {
      return;
    }
    const auto joined = join_client_path(prefix, url.path);
    if (!joined) {
      return;
    }
    out.push_back(ClientCall{.kind = ClientCall::Kind::Wrapper, .client = std::move(client), .method = method,
                             .path = *joined, .tail = url.tail_index});
    return;
  }
  if (method.parameter >= 0 || method.unknown || !url.resolvable || url.base_index >= 0) {
    unresolved();
    return;
  }
  const auto joined = join_client_path(prefix, url.path);
  const auto path = joined && !joined->empty() && (joined->front() == '/' || joined->starts_with("${")) ? *joined : std::string{};
  if (!method.choices.empty()) {
    for (const auto& choice : method.choices) {
      out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = client, .method = {.verb = choice}, .path = path});
    }
    return;
  }
  if (method.verb.empty()) {
    method.verb = "GET";
  }
  out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = std::move(client), .method = method, .path = path});
}

// The base URL a client instance carries: `axios.create({ baseURL })` held in
// a module constant or a class member (`this.api`). Empty optional for any
// other receiver; `resolvable` false when the instance's base URL may hold a
// path this file cannot read. The prefix always ends in a slash: axios joins
// baseURL and url with one.
struct ReceiverBase {
  bool resolvable = true;
  std::string prefix;
};

[[nodiscard]] std::optional<ReceiverBase> receiver_base(const TSNode& receiver, const ExtractionContext& context) {
  TSNode value{};
  const std::string_view type = ts_node_type(receiver);
  if (type == "identifier") {
    value = module_const_value(receiver, node_text(receiver, context.source), context.source);
  } else if (type == "member_expression") {
    const TSNode object = unwrap_expression(ts_node_child_by_field_name(receiver, "object", 6));
    if (!ts_node_is_null(object) && std::string_view(ts_node_type(object)) == "this") {
      if (const TSNode body = enclosing_class_body(receiver); !ts_node_is_null(body)) {
        value = class_member_value(body, field_text(receiver, "property", context.source), context.source);
      }
    }
  }
  if (ts_node_is_null(value) || std::string_view(ts_node_type(value)) != "call_expression" ||
      node_text(ts_node_child_by_field_name(value, "function", 8), context.source) != "axios.create") {
    return std::nullopt;
  }
  const TSNode arguments = ts_node_child_by_field_name(value, "arguments", 9);
  if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0) {
    return std::nullopt;
  }
  const TSNode options = unwrap_expression(ts_node_named_child(arguments, 0));
  if (std::string_view(ts_node_type(options)) != "object") {
    return ReceiverBase{.resolvable = false};
  }
  const auto count = ts_node_named_child_count(options);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode pair = ts_node_named_child(options, index);
    if (std::string_view(ts_node_type(pair)) != "pair" ||
        strip_string_quotes(field_text(pair, "key", context.source)) != "baseURL") {
      continue;
    }
    UrlTemplate base;
    collect_url_template(ts_node_child_by_field_name(pair, "value", 5), context, {}, base, 0);
    if (!base.resolvable || base.tail || base.base_index >= 0 || base.dropped_host || base.in_query ||
        base.path.starts_with("http://") || base.path.starts_with("https://")) {
      return ReceiverBase{.resolvable = false};
    }
    auto prefix = std::move(base.path);
    if (prefix.empty() || prefix.back() != '/') {
      prefix.push_back('/');
    }
    return ReceiverBase{.prefix = std::move(prefix)};
  }
  return std::nullopt;
}

// `url.toString()` and `url.href` are the URL itself.
[[nodiscard]] TSNode peel_url_argument(TSNode node, std::string_view source) {
  for (;;) {
    node = unwrap_expression(node);
    if (ts_node_is_null(node)) {
      return node;
    }
    const std::string_view type = ts_node_type(node);
    if (type == "member_expression" && field_text(node, "property", source) == "href") {
      node = ts_node_child_by_field_name(node, "object", 6);
      continue;
    }
    if (type == "call_expression") {
      const TSNode callee = unwrap_expression(ts_node_child_by_field_name(node, "function", 8));
      const TSNode arguments = ts_node_child_by_field_name(node, "arguments", 9);
      if (!ts_node_is_null(callee) && std::string_view(ts_node_type(callee)) == "member_expression" &&
          field_text(callee, "property", source) == "toString" && !ts_node_is_null(arguments) &&
          ts_node_named_child_count(arguments) == 0) {
        node = ts_node_child_by_field_name(callee, "object", 6);
        continue;
      }
    }
    return node;
  }
}

// Every argument of a call to a function this file does not define, in the
// form resolve_contracts reads when that function turns out to be a wrapper
// whose path is not its first parameter or whose method is a parameter:
// `P<path>` a resolvable path, `V<VERB>` a verb literal, `O<VERB>` an options
// object with a literal method, empty for anything else; tab-separated.
[[nodiscard]] std::optional<std::string> argument_descriptors(const TSNode& arguments, const UrlScope& scope,
                                                              const ExtractionContext& context) {
  const auto count = ts_node_named_child_count(arguments);
  std::string descriptors;
  bool later_path = false;
  bool first_path = false;
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode argument = unwrap_expression(ts_node_named_child(arguments, index));
    const std::string_view type = ts_node_type(argument);
    std::string descriptor;
    if (type == "string" || type == "template_string" || type == "binary_expression") {
      UrlTemplate url;
      collect_url_template(argument, context, scope, url, 0);
      url.finish(true);
      if (url.resolvable && !url.tail && url.base_index < 0 && !url.path.empty()) {
        descriptor = "P" + url.path;
        (index == 0 ? first_path : later_path) = true;
      } else if (auto literal = literal_verb(argument, context.source); !literal.empty()) {
        descriptor = "V" + literal;
      }
    } else if (type == "object") {
      if (auto method = options_method(argument, scope.parameters, context.source); !method.verb.empty() && !method.unknown) {
        descriptor = "O" + method.verb;
      }
    }
    if (index > 0) {
      descriptors.push_back('\t');
    }
    descriptors += descriptor;
  }
  if (!later_path && !first_path) {
    return std::nullopt;
  }
  return descriptors;
}

// `fetch(url, opts)`, `api.GET('/path')`, `axios.post(url)` and calls to a
// wrapper with a path-like argument yield consumers; a function whose own
// client call appends one of its parameters to a fixed prefix yields a
// wrapper instead of a call of its own. `this.method(...)` into a wrapper
// method of the same class, and a call forwarding a parameter into a wrapper
// function of this file, are read through that wrapper here.
std::vector<ClientCall> analyze_client_call(const TSNode& node, const ExtractionContext& context, int depth) {
  std::vector<ClientCall> out;
  if (std::string_view(ts_node_type(node)) != "call_expression") {
    return out;
  }
  TSNode callee = unwrap_expression(ts_node_child_by_field_name(node, "function", 8));
  // tree-sitter-typescript parses `await axios.post<T>(url)` as `(await axios.post)<T>(url)`:
  // with type arguments the await wraps the callee, not the call. Read through it,
  // or every typed awaited request (`await axios.post<LoginResponse>(...)`) is lost.
  if (!ts_node_is_null(callee) && std::string_view(ts_node_type(callee)) == "await_expression" &&
      !ts_node_is_null(ts_node_child_by_field_name(node, "type_arguments", 14))) {
    callee = unwrap_expression(ts_node_named_child(callee, 0));
  }
  if (ts_node_is_null(callee)) {
    return out;
  }
  const TSNode arguments = ts_node_child_by_field_name(node, "arguments", 9);
  if (ts_node_is_null(arguments)) {
    return out;
  }
  const auto argument_count = ts_node_named_child_count(arguments);
  if (argument_count == 0) {
    return out;
  }
  for (std::uint32_t index = 0; index < argument_count; ++index) {
    if (is_function_value(ts_node_named_child(arguments, index))) {
      return out;  // a handler argument: this is a route registration (or a callback API), not a request
    }
  }
  const UrlScope scope{.parameters = enclosing_parameters(node, context.source)};
  std::string client;
  std::string verb;
  TSNode receiver{};
  const std::string_view callee_type = ts_node_type(callee);
  if (callee_type == "identifier") {
    client = node_text(callee, context.source);
  } else if (callee_type == "member_expression") {
    const auto property = field_text(callee, "property", context.source);
    receiver = unwrap_expression(ts_node_child_by_field_name(callee, "object", 6));
    if (ts_node_is_null(receiver)) {
      return out;
    }
    // `this.patch<User>(\`v1/users/${id}/enable\`)`: a method of this class.
    if (std::string_view(ts_node_type(receiver)) == "this") {
      if (depth >= kMaxWrapperDepth) {
        return out;
      }
      const TSNode body = enclosing_class_body(node);
      const TSNode method = ts_node_is_null(body) ? TSNode{} : class_method(body, property, context.source);
      if (ts_node_is_null(method)) {
        return out;
      }
      if (const auto shape = wrapper_shape(method, context, depth + 1)) {
        compose_wrapper_call(*shape, "this." + property, arguments, scope, context, out);
      }
      return out;
    }
    std::string lower(property);
    for (auto& ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (!is_http_verb(lower) || lower == "all") {
      return out;
    }
    const std::string_view receiver_type = ts_node_type(receiver);
    const auto receiver_name = receiver_type == "identifier"
                                   ? node_text(receiver, context.source)
                                   : receiver_type == "member_expression" ? field_text(receiver, "property", context.source)
                                                                          : std::string{};
    if (!looks_like_http_client(receiver_name)) {
      return out;
    }
    client = receiver_name + "." + property;
    verb = lower;
  } else {
    return out;
  }
  const bool primitive = client == "fetch" || !verb.empty();
  if (!primitive && depth < kMaxWrapperDepth) {
    // A parameter forwarded into a wrapper this file defines: the enclosing
    // function is a wrapper in turn, read through the inner one.
    bool forwards = false;
    for (std::uint32_t index = 0; index < argument_count && !forwards; ++index) {
      const TSNode argument = unwrap_expression(ts_node_named_child(arguments, index));
      forwards = std::string_view(ts_node_type(argument)) == "identifier" &&
                 parameter_index(scope.parameters, node_text(argument, context.source)) >= 0;
    }
    if (forwards) {
      if (const TSNode function = module_function(node, client, context.source); !ts_node_is_null(function)) {
        if (const auto shape = wrapper_shape(function, context, depth + 1)) {
          std::vector<ClientCall> composed;
          compose_wrapper_call(*shape, client, arguments, scope, context, composed);
          for (auto& outcome : composed) {
            if (outcome.kind == ClientCall::Kind::Wrapper) {
              out.push_back(std::move(outcome));
            }
          }
          if (!out.empty()) {
            return out;
          }
        }
      }
    }
  }
  const TSNode first = peel_url_argument(ts_node_named_child(arguments, 0), context.source);
  const std::string_view first_type = ts_node_is_null(first) ? std::string_view{} : ts_node_type(first);
  if (first_type != "string" && first_type != "template_string" && first_type != "identifier" &&
      first_type != "binary_expression" &&
      (first_type != "new_expression" || field_text(first, "constructor", context.source) != "URL")) {
    return out;
  }
  // `let url; if (a) url = x; else url = y; fetch(url)`: one consumer per value.
  std::vector<TSNode> values{first};
  if (primitive && first_type == "identifier") {
    if (const auto locals = local_values(first, node_text(first, context.source), context.source);
        locals && locals->size() > 1) {
      values = *locals;
    }
  }
  std::optional<ReceiverBase> base;
  if (!ts_node_is_null(receiver)) {
    base = receiver_base(receiver, context);
  }
  MethodSource method = argument_count >= 2
                            ? options_method(ts_node_named_child(arguments, 1), scope.parameters, context.source)
                            : MethodSource{};
  if (!verb.empty()) {
    method.options = -1;  // `axios.post(url, data)`: the second argument is the body, the verb is fixed
    method.unknown = false;
  } else if (!primitive) {
    method.unknown = false;  // a wrapper's second argument may be anything (a body); contracts read its method
  }
  for (const auto& value : values) {
    UrlTemplate url;
    collect_url_template(value, context, scope, url, 0);
    url.finish(!primitive || base.has_value());
    if (base) {
      if (!base->resolvable) {
        url.resolvable = false;
      } else if (url.resolvable) {
        const auto joined = join_client_path(base->prefix, url.path);
        if (joined) {
          url.path = *joined;
          url.relative = false;
        } else {
          url.resolvable = false;
        }
      }
    }
    if (url.tail) {
      // This call appends one of the enclosing function's parameters: the
      // function is a wrapper, and callers of it are the consumers.
      if (primitive && url.resolvable) {
        MethodSource fixed = method;
        if (fixed.verb.empty() && fixed.parameter < 0 && !verb.empty()) {
          fixed.verb = upper_verb(verb);
        }
        out.push_back(ClientCall{.kind = ClientCall::Kind::Wrapper, .client = client, .method = fixed,
                                 .path = url.path, .tail = url.tail_index, .base = url.base_index});
      }
      continue;
    }
    if (url.base_index >= 0) {
      url.resolvable = false;  // a base only a caller supplies: not a consumer on its own
    }
    if (!primitive && (!url.resolvable || url.path.empty())) {
      continue;  // a function taking some string: only a path-like literal marks a wrapper call
    }
    if (!method.choices.empty() && url.resolvable) {
      for (const auto& choice : method.choices) {
        out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = client,
                                 .method = MethodSource{.verb = choice}, .path = url.path});
      }
      continue;
    }
    // Options whose method this file cannot read: the call is unresolved.
    out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = client,
                             .method = MethodSource{.verb = method.verb},
                             .path = url.resolvable && !method.unknown ? url.path : std::string{}});
  }
  if (!primitive) {
    if (auto descriptors = argument_descriptors(arguments, scope, context)) {
      out.push_back(ClientCall{.kind = ClientCall::Kind::Arguments, .client = client, .path = std::move(*descriptors)});
    }
  }
  return out;
}

// The verb literal a parameter of the function `node` sits in defaults to
// (`method = 'GET'`), by position; empty when it has none.
[[nodiscard]] std::string parameter_default_verb(const TSNode& node, int position, std::string_view source) {
  const TSNode function = enclosing_function(node);
  const TSNode parameters = ts_node_is_null(function) ? TSNode{} : ts_node_child_by_field_name(function, "parameters", 10);
  if (ts_node_is_null(parameters)) {
    return {};
  }
  int at = 0;
  const auto count = ts_node_named_child_count(parameters);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode parameter = ts_node_named_child(parameters, index);
    if (std::string_view(ts_node_type(parameter)) == "comment") {
      continue;
    }
    if (at++ != position) {
      continue;
    }
    TSNode value = ts_node_child_by_field_name(parameter, "value", 5);
    if (ts_node_is_null(value) && std::string_view(ts_node_type(parameter)) == "assignment_pattern") {
      value = ts_node_child_by_field_name(parameter, "right", 5);
    }
    return ts_node_is_null(value) ? std::string{} : literal_verb(value, source);
  }
  return {};
}

}  // namespace

struct HttpConsumerFileScope::Cache {
  ShapeCache shapes;
  ShapeCache* previous = nullptr;
};

HttpConsumerFileScope::HttpConsumerFileScope() : cache_(std::make_unique<Cache>()) {
  cache_->previous = active_shapes;
  active_shapes = &cache_->shapes;
}

HttpConsumerFileScope::~HttpConsumerFileScope() {
  active_shapes = cache_->previous;
}

// Records the facts analyze_client_call finds: `http_call` for a consumer,
// `http_wrapper` for a wrapper whose slots resolve_contracts can fill (its
// callers supply the path and, when it is a parameter, the method; a base
// parameter is only filled by a wrapper of this file forwarding into it), and
// `http_call_args` for a call whose wrapper may take the path elsewhere.
void http_call_handler(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                       std::vector<RawRelation>& out) {
  for (auto& call : analyze_client_call(node, context, 0)) {
    switch (call.kind) {
      case ClientCall::Kind::Wrapper: {
        if (function_scope_id.empty() || call.base >= 0) {
          break;
        }
        std::string fact = call.method.verb;
        if (call.method.parameter >= 0) {
          fact = "@" + std::to_string(call.method.parameter);
          if (auto fallback = parameter_default_verb(node, call.method.parameter, context.source); !fallback.empty()) {
            fact += "=" + fallback;  // `request(path, method = 'GET')`: a call leaving it out means GET
          }
        }
        fact += " " + call.path;
        if (call.tail != 0) {
          fact += " #" + std::to_string(call.tail);
        }
        out.push_back(RawRelation{
            .source_id = function_scope_id,
            .target_label = std::move(call.client),
            .relation = "http_wrapper",
            .context = std::move(fact),
            .source_file = context.source_file,
        });
        break;
      }
      case ClientCall::Kind::Consumer:
        out.push_back(RawRelation{
            .source_id = consumer_scope_id(node, context, function_scope_id),
            .target_label = std::move(call.client),
            .relation = "http_call",
            .context = call.method.verb + " " + call.path,
            .source_file = context.source_file,
        });
        break;
      case ClientCall::Kind::Arguments:
        out.push_back(RawRelation{
            .source_id = consumer_scope_id(node, context, function_scope_id),
            .target_label = std::move(call.client),
            .relation = "http_call_args",
            .context = std::move(call.path),
            .source_file = context.source_file,
        });
        break;
    }
  }
}

// Module-level string constants that read as a URL or a URL prefix
// (`export const API_BASE = \`${API_URL}/api/v1\``, `const API_URL =
// process.env.X || 'http://localhost'`) record `url_const` facts: name and the
// path they hold (empty for a bare host), so a wrapper in another file that
// appends its argument to an imported base resolves the base project-wide.
void url_const_handler(const TSNode& node, const ExtractionContext& context, std::vector<RawRelation>& out) {
  const std::string_view type = ts_node_type(node);
  if ((type != "lexical_declaration" && type != "variable_declaration") || !is_module_level_declaration(node)) {
    return;
  }
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode declarator = ts_node_named_child(node, index);
    if (std::string_view(ts_node_type(declarator)) != "variable_declarator") {
      continue;
    }
    const TSNode value = unwrap_expression(ts_node_child_by_field_name(declarator, "value", 5));
    if (ts_node_is_null(value)) {
      continue;
    }
    const std::string_view value_type = ts_node_type(value);
    // `process.env.X || 'http://localhost'`, `(process.env.X || '…').replace(/\/$/, '')`:
    // however it is trimmed, an environment-derived value is a host.
    const bool mentions_env = node_text(value, context.source).find("process.env.") != std::string::npos;
    std::string path;
    bool is_url = false;
    if (value_type == "string" || value_type == "template_string" || value_type == "binary_expression") {
      UrlTemplate url;
      collect_url_template(value, context, {}, url, 0);
      url.finish();
      if (url.resolvable && !url.tail && url.base_index < 0) {
        path = url.path;
        is_url = true;
      }
    }
    if (!is_url && mentions_env) {
      is_url = true;  // a bare host
    }
    if (!is_url) {
      continue;  // `const TITLE = 'Hello'`: a string, not a URL
    }
    out.push_back(RawRelation{
        .source_id = make_id(context.relative_path),
        .target_label = field_text(declarator, "name", context.source),
        .relation = "url_const",
        .context = std::move(path),
        .source_file = context.source_file,
    });
  }
}

}  // namespace cgraph
