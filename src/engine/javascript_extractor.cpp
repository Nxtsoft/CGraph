#include "cgraph/javascript_extractor.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

extern "C" const TSLanguage* tree_sitter_javascript();
extern "C" const TSLanguage* tree_sitter_typescript();
extern "C" const TSLanguage* tree_sitter_tsx();

[[nodiscard]] std::string node_text(const TSNode& node, std::string_view source) {
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  if (start >= end || end > source.size()) {
    return {};
  }
  return std::string(source.substr(start, end - start));
}

[[nodiscard]] SourceLocation source_location(const TSNode& node) {
  const auto start = ts_node_start_point(node);
  const auto end = ts_node_end_point(node);
  return SourceLocation{
      .start_line = start.row + 1,
      .start_column = start.column,
      .end_line = end.row + 1,
      .end_column = end.column,
  };
}

[[nodiscard]] std::string field_text(const TSNode& node, const char* field, std::string_view source) {
  const auto child = ts_node_child_by_field_name(node, field, static_cast<std::uint32_t>(std::string_view(field).size()));
  return ts_node_is_null(child) ? std::string{} : node_text(child, source);
}

[[nodiscard]] std::string strip_string_quotes(std::string value);

// The verbs a router DSL exposes as methods. Elysia, Express, Hono, Fastify and
// koa-router all register a route as `<router>.<verb>('<path>', ..., handler)`.
constexpr std::array<std::string_view, 8> kRouteVerbs = {
    "get", "post", "put", "patch", "delete", "head", "options", "all",
};

[[nodiscard]] bool is_function_value(const TSNode& node) {
  const std::string_view type = ts_node_type(node);
  return type == "arrow_function" || type == "function_expression";
}

[[nodiscard]] bool is_string_value(const TSNode& node) {
  const std::string_view type = ts_node_type(node);
  return type == "string" || type == "template_string";
}

// TypeScript wrappers that change an expression's static type but not its value:
// `x as any`, `x satisfies T`, `x!`, `(x)`, `<T>x`. A router chain is routinely
// wrapped in these (`app.use(apiRoutes as any)`, `new Elysia().use(a) as unknown
// as Elysia` to dodge TS2589), and the chain must read through them.
[[nodiscard]] bool is_type_wrapper(std::string_view type) {
  return type == "as_expression" || type == "satisfies_expression" || type == "non_null_expression" ||
         type == "parenthesized_expression" || type == "type_assertion";
}

[[nodiscard]] TSNode unwrap_expression(TSNode node) {
  while (!ts_node_is_null(node) && is_type_wrapper(ts_node_type(node))) {
    const auto count = ts_node_named_child_count(node);
    if (count == 0) {
      break;
    }
    // `<T>x` puts the type first; every other wrapper puts the expression first.
    node = ts_node_named_child(node, std::string_view(ts_node_type(node)) == "type_assertion" ? count - 1 : 0);
  }
  return node;
}

[[nodiscard]] std::string chain_route_prefix(const TSNode& value, std::string_view source);

// Where a fluent router chain is rooted, for resolve_contracts (contracts.hpp).
struct ChainRef {
  std::string root;     // the module-level identifier the chain hangs off; empty when unknown
  std::string prefix;   // path accumulated from enclosing `.group('/p')`, `.route('/p', …)` and inline constructors
  bool resolvable = true;
};

[[nodiscard]] std::string compose_prefix(const std::string& outer, const std::string& inner) {
  if (outer.empty() && inner.empty()) {
    return {};
  }
  return join_route_path(outer, inner);
}

[[nodiscard]] bool is_function_node(std::string_view type) {
  return type == "arrow_function" || type == "function_expression" || type == "function_declaration" ||
         type == "method_definition" || type == "generator_function_declaration";
}

// The identifiers a function's parameter list binds: `(app) => …`, `app => …`,
// `function (req, res) {}`. Destructured parameters bind no chain and add nothing.
void parameter_names(const TSNode& function, std::string_view source, std::vector<std::string>& out) {
  if (const TSNode single = ts_node_child_by_field_name(function, "parameter", 9); !ts_node_is_null(single)) {
    if (std::string_view(ts_node_type(single)) == "identifier") {
      out.push_back(node_text(single, source));
    }
    return;
  }
  const TSNode parameters = ts_node_child_by_field_name(function, "parameters", 10);
  if (ts_node_is_null(parameters)) {
    return;
  }
  const auto count = ts_node_named_child_count(parameters);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode parameter = ts_node_named_child(parameters, index);
    if (std::string_view(ts_node_type(parameter)) == "identifier") {
      out.push_back(node_text(parameter, source));
      continue;
    }
    if (const TSNode pattern = ts_node_child_by_field_name(parameter, "pattern", 7);
        !ts_node_is_null(pattern) && std::string_view(ts_node_type(pattern)) == "identifier") {
      out.push_back(node_text(pattern, source));
    }
  }
}

// True when a statement directly in `body` declares `name` with `const` / `let`
// / `var`: a chain built inside a test callback (`describe(() => { const app =
// new Elysia(); app.get(...) })`) is local to it, and must not bind to a
// module-level variable of the same name. Nested blocks are not searched.
[[nodiscard]] bool declares_local(const TSNode& body, std::string_view name, std::string_view source) {
  if (ts_node_is_null(body) || std::string_view(ts_node_type(body)) != "statement_block") {
    return false;
  }
  const auto count = ts_node_named_child_count(body);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode statement = ts_node_named_child(body, index);
    const std::string_view type = ts_node_type(statement);
    if (type != "lexical_declaration" && type != "variable_declaration") {
      continue;
    }
    const auto declarators = ts_node_named_child_count(statement);
    for (std::uint32_t d = 0; d < declarators; ++d) {
      const TSNode declarator = ts_node_named_child(statement, d);
      if (std::string_view(ts_node_type(declarator)) == "variable_declarator" &&
          field_text(declarator, "name", source) == name) {
        return true;
      }
    }
  }
  return false;
}

// The outermost node of the chain `call` belongs to: `a.use(x).get(…)` read up
// through every member access, call and type wrapper.
[[nodiscard]] TSNode chain_top(const TSNode& call) {
  TSNode top = call;
  for (TSNode parent = ts_node_parent(top); !ts_node_is_null(parent); parent = ts_node_parent(top)) {
    const std::string_view type = ts_node_type(parent);
    if (type != "member_expression" && type != "call_expression" && !is_type_wrapper(type)) {
      break;
    }
    top = parent;
  }
  return top;
}

// The router method (`use`, `group`, …) of the call whose argument list holds
// `argument`, or empty when the argument is not inside such a call. `call` and
// `arguments` receive the enclosing call and its argument list.
[[nodiscard]] std::string enclosing_router_call(const TSNode& argument, std::string_view source, TSNode& call, TSNode& arguments) {
  arguments = ts_node_parent(argument);
  if (ts_node_is_null(arguments) || std::string_view(ts_node_type(arguments)) != "arguments") {
    return {};
  }
  call = ts_node_parent(arguments);
  if (ts_node_is_null(call) || std::string_view(ts_node_type(call)) != "call_expression") {
    return {};
  }
  const TSNode callee = ts_node_child_by_field_name(call, "function", 8);
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression") {
    return {};
  }
  return field_text(callee, "property", source);
}

// The string a router call mounts under: the first argument of `.group('/p', …)`,
// `.route('/p', x)` or `.use('/p', x)` when it is a string and not `argument` itself.
[[nodiscard]] std::string mount_path_argument(const TSNode& arguments, const TSNode& argument, std::string_view source) {
  if (ts_node_named_child_count(arguments) < 2) {
    return {};
  }
  const TSNode first = ts_node_named_child(arguments, 0);
  if (!is_string_value(first) || ts_node_eq(first, argument)) {
    return {};
  }
  return strip_string_quotes(node_text(first, source));
}

// The identifier a fluent chain hangs off, and the path it is served under
// relative to that identifier's own chain. `app.get(...)` and
// `app.use(x).get(...)` both reduce to `app`. A chain rooted in a constructor
// (`new Elysia({...}).use(x).get(...)`) has no identifier: the variable it is
// assigned to names it (`const notebookRoutes = ...`); passed inline to an
// enclosing chain's `.use(...)` / `.route('/p', ...)` it belongs to that chain,
// under the mount path and its own constructor prefix. A chain that is the
// parameter of a `.group('/p', app => ...)` / `.guard(opts, app => ...)`
// callback is the enclosing chain under the group's path. A parameter of any
// other function (`function register(app) { app.get(...) }`), or an unassigned
// chain in an expression statement, is unresolvable: its mount is unknowable
// from this file, and a guessed path would be worse than none.
[[nodiscard]] ChainRef resolve_chain(const TSNode& call, const ExtractionContext& context, int depth = 0) {
  constexpr int kMaxNesting = 8;
  if (depth > kMaxNesting) {
    return ChainRef{.resolvable = false};
  }
  TSNode base = call;
  for (;;) {
    const std::string_view type = ts_node_type(base);
    TSNode next;
    if (type == "call_expression") {
      next = ts_node_child_by_field_name(base, "function", 8);
    } else if (type == "member_expression") {
      next = ts_node_child_by_field_name(base, "object", 6);
    } else if (is_type_wrapper(type)) {
      next = unwrap_expression(base);
    } else {
      break;
    }
    if (ts_node_is_null(next) || ts_node_eq(next, base)) {
      break;
    }
    base = next;
  }
  if (std::string_view(ts_node_type(base)) == "identifier") {
    const auto name = node_text(base, context.source);
    for (TSNode ancestor = ts_node_parent(call); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
      if (!is_function_node(ts_node_type(ancestor))) {
        continue;
      }
      if (declares_local(ts_node_child_by_field_name(ancestor, "body", 4), name, context.source)) {
        return ChainRef{.resolvable = false};  // a chain local to this function: its mount is unknowable
      }
      std::vector<std::string> parameters;
      parameter_names(ancestor, context.source, parameters);
      if (std::ranges::find(parameters, name) == parameters.end()) {
        continue;
      }
      TSNode outer;
      TSNode arguments;
      const auto method = enclosing_router_call(ancestor, context.source, outer, arguments);
      if (method != "group" && method != "guard") {
        return ChainRef{.resolvable = false};
      }
      auto enclosing = resolve_chain(outer, context, depth + 1);
      enclosing.prefix = compose_prefix(
          enclosing.prefix, method == "group" ? mount_path_argument(arguments, ancestor, context.source) : std::string{});
      return enclosing;
    }
    return ChainRef{.root = name};
  }
  const TSNode top = chain_top(call);
  const TSNode parent = ts_node_parent(top);
  if (ts_node_is_null(parent)) {
    return ChainRef{.resolvable = false};
  }
  if (std::string_view(ts_node_type(parent)) == "variable_declarator") {
    auto name = field_text(parent, "name", context.source);
    if (name.empty()) {
      return ChainRef{.resolvable = false};
    }
    return ChainRef{.root = std::move(name)};
  }
  TSNode outer;
  TSNode arguments;
  const auto method = enclosing_router_call(top, context.source, outer, arguments);
  if (method != "use" && method != "route") {
    return ChainRef{.resolvable = false};
  }
  auto enclosing = resolve_chain(outer, context, depth + 1);
  enclosing.prefix = compose_prefix(compose_prefix(enclosing.prefix, mount_path_argument(arguments, top, context.source)),
                                    chain_route_prefix(top, context.source));
  return enclosing;
}

// Names the inline handler of an HTTP route registration from the call that
// registers it. In `notebookRoutes.get('/starred-notes', async (ctx) => {...},
// {detail})` the last function-valued argument is the handler and is labelled
// `notebookRoutes.get /starred-notes`; earlier function arguments (Express
// middleware) stay anonymous. Before this, an Elysia module was one `variable`
// node spanning every route (turing-api's `notebookRoutes`, 220 lines), so a
// source anchor could name the module but never the handler, and the calls
// inside every handler were dropped at the arrow boundary. Returns empty for a
// call that is not a route registration: the callee must be `<x>.<verb>` with
// an HTTP verb, the first argument a string, and `node` the last function
// argument.
struct RouteRegistration {
  std::string root;  // the chain's identifier, empty when unknown
  std::string verb;  // lowercase, as the router method is spelled
  std::string path;  // as written (quotes stripped), beneath any enclosing group / inline prefix
  bool resolvable = true;
};

[[nodiscard]] std::optional<RouteRegistration> route_registration(const TSNode& node, const ExtractionContext& context) {
  if (!is_function_value(node)) {
    return std::nullopt;
  }
  const TSNode arguments = ts_node_parent(node);
  if (ts_node_is_null(arguments) || std::string_view(ts_node_type(arguments)) != "arguments") {
    return std::nullopt;
  }
  const TSNode call = ts_node_parent(arguments);
  if (ts_node_is_null(call) || std::string_view(ts_node_type(call)) != "call_expression") {
    return std::nullopt;
  }
  const TSNode callee = ts_node_child_by_field_name(call, "function", 8);
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression") {
    return std::nullopt;
  }
  auto verb = field_text(callee, "property", context.source);
  if (std::ranges::find(kRouteVerbs, std::string_view(verb)) == kRouteVerbs.end()) {
    return std::nullopt;
  }
  const auto argument_count = ts_node_named_child_count(arguments);
  if (argument_count < 2) {
    return std::nullopt;
  }
  const TSNode path = ts_node_named_child(arguments, 0);
  if (!is_string_value(path)) {
    return std::nullopt;
  }
  TSNode handler;
  for (std::uint32_t index = argument_count; index-- > 1;) {
    const TSNode argument = ts_node_named_child(arguments, index);
    if (is_function_value(argument)) {
      handler = argument;
      break;
    }
  }
  if (ts_node_is_null(handler) || !ts_node_eq(handler, node)) {
    return std::nullopt;
  }
  auto chain = resolve_chain(call, context);
  auto route = strip_string_quotes(node_text(path, context.source));
  if (!chain.prefix.empty()) {
    route = join_route_path(chain.prefix, route);
  }
  return RouteRegistration{
      .root = std::move(chain.root),
      .verb = std::move(verb),
      .path = std::move(route),
      .resolvable = chain.resolvable,
  };
}

[[nodiscard]] std::string route_handler_name(const TSNode& node, const ExtractionContext& context) {
  const auto registration = route_registration(node, context);
  if (!registration) {
    return {};
  }
  const auto& root = registration->root;
  return (root.empty() ? registration->verb : root + "." + registration->verb) + " " + registration->path;
}

// Names an arrow function / function expression from the construct it is bound
// to (`const Foo = () => {}`, `{ handler: () => {} }`, `this.x = () => {}`,
// class fields, an HTTP route's inline handler). Returns empty for genuinely
// anonymous functions so the caller skips them. Named function declarations and
// methods return empty here and are named by the generic name_fields path
// instead.
[[nodiscard]] std::string resolve_js_function_name(const TSNode& node, const ExtractionContext& context) {
  const std::string_view type = ts_node_type(node);
  if (type != "arrow_function" && type != "function_expression") {
    return {};
  }
  const auto parent = ts_node_parent(node);
  if (ts_node_is_null(parent)) {
    return {};
  }
  const std::string_view parent_type = ts_node_type(parent);
  if (parent_type == "variable_declarator") {
    return field_text(parent, "name", context.source);
  }
  if (parent_type == "pair") {
    return field_text(parent, "key", context.source);
  }
  if (parent_type == "assignment_expression") {
    return field_text(parent, "left", context.source);
  }
  if (parent_type == "public_field_definition" || parent_type == "field_definition") {
    auto name = field_text(parent, "name", context.source);
    return name.empty() ? field_text(parent, "property", context.source) : name;
  }
  if (parent_type == "arguments") {
    return route_handler_name(node, context);
  }
  return {};
}

// A route's inline handler is the one nested arrow that is a call scope: its
// body is the endpoint's implementation, and attributing `s.listNotebooks()`
// to the handler instead of dropping it is what makes the route reachable.
[[nodiscard]] bool is_route_handler(const TSNode& node, const ExtractionContext& context) {
  return !route_handler_name(node, context).empty();
}

[[nodiscard]] std::string strip_string_quotes(std::string value) {
  if (value.size() >= 2) {
    const char front = value.front();
    if ((front == '\'' || front == '"' || front == '`') && value.back() == front) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

// Relative specifiers ("./x", "../y") resolve against the importing file's
// directory so every file importing the same module lands on one shared module
// hub node; bare package specifiers ("react") are already shared by name.
[[nodiscard]] std::string resolve_module_spec(const std::string& source_file, const std::string& spec) {
  if (!spec.empty() && spec.front() == '.') {
    const std::filesystem::path base = std::filesystem::path(source_file).parent_path();
    return (base / spec).lexically_normal().generic_string();
  }
  return spec;
}

// Pushes the imported/exported symbol names found under an import_clause /
// export_clause / namespace_(im|ex)port subtree, each with the local alias an
// `as` clause gives it (empty when there is none). The stub is keyed by the
// imported name, since that is what the source module declares; the alias is
// what the importing file's own code refers to.
void collect_specifier_names(const TSNode& node, std::string_view source, std::vector<std::pair<std::string, std::string>>& out) {
  const std::string_view type = ts_node_type(node);
  if (type == "import_specifier" || type == "export_specifier") {
    if (const auto name = ts_node_child_by_field_name(node, "name", 4); !ts_node_is_null(name)) {
      auto text = node_text(name, source);
      if (!text.empty()) {
        out.emplace_back(std::move(text), field_text(node, "alias", source));
      }
    }
    return;  // do not descend into the alias
  }
  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const auto child = ts_node_child(node, index);
    const std::string_view child_type = ts_node_type(child);
    // Default import (`import Foo from`) and namespace import (`* as ns`) both
    // surface as a bare identifier directly under the import clause.
    if (child_type == "identifier") {
      auto text = node_text(child, source);
      if (!text.empty()) {
        out.emplace_back(std::move(text), std::string{});
      }
      continue;
    }
    collect_specifier_names(child, source, out);
  }
}

void module_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  const std::string_view statement_type = ts_node_type(node);
  const bool is_export = statement_type == "export_statement";
  const auto source = ts_node_child_by_field_name(node, "source", 6);
  const bool has_source = !ts_node_is_null(source);

  // `export class Foo {}` / `export const x = ...` has no source module: the
  // declaration is extracted (and `contains`-linked) by the normal walk, so the
  // export statement itself adds nothing.
  if (is_export && !has_source) {
    return;
  }

  const std::string file_id = make_id(context.source_file);
  const std::string module_relation = is_export ? "re_exports" : "imports_from";
  const std::string symbol_relation = is_export ? "re_exports" : "imports";

  if (has_source) {
    const auto spec = strip_string_quotes(node_text(source, context.source));
    if (!spec.empty()) {
      const auto resolved = resolve_module_spec(context.source_file, spec);
      // The stub id is namespaced so it can never equal a real node's id. A
      // specifier that spells the source extension ("./chunkBy.ts", legal under
      // allowImportingTsExtensions) resolves to the imported file's exact path,
      // and an un-namespaced make_id(resolved) would collide with that file
      // node's id. Fragments merge in path order and "X.spec.ts" sorts before
      // "X.ts", so the stub claimed the id first, merge_fragment discarded the
      // real file node as a duplicate, and resolve_imports — finding no file
      // node for the path — deleted the stub and every edge with it (issue #39).
      const auto module_id = make_id("import-module:" + resolved);
      fragment.nodes.push_back(Node{
          .id = module_id,
          .label = spec,
          .source_location = SourceLocation{.start_line = 1, .end_line = 1},
          .kind = "module",
          .confidence = Confidence::Extracted,
          // Recorded so a post-merge pass can resolve this module to the real
          // project file it refers to (collapsing the stub onto that file node).
          .properties = {{"import_path", resolved}},
      });
      // file -> source module: `imports_from` for imports, `re_exports` for
      // `export ... from` statements.
      fragment.edges.push_back(Edge{
          .source = file_id,
          .target = module_id,
          .relation = module_relation,
          .confidence = Confidence::Extracted,
      });
    }
  }

  // file -> each named/default/namespace symbol. Keyed by module+name so the
  // same symbol imported by many files collapses onto one hub node.
  const auto module_key = has_source
      ? resolve_module_spec(context.source_file, strip_string_quotes(node_text(source, context.source)))
      : context.source_file;
  std::vector<std::pair<std::string, std::string>> names;
  collect_specifier_names(node, context.source, names);
  for (auto& [name, alias] : names) {
    // Namespaced for the same reason as module_id above: with an
    // extension-spelled specifier, make_id(module_key + ":" + name) is exactly
    // the id add_symbol_node gives the real declared symbol, and the squatting
    // stub deletes the real function from the graph (issue #39/#40).
    const auto symbol_id = make_id("import-symbol:" + module_key + ":" + name);
    fragment.nodes.push_back(Node{
        .id = symbol_id,
        .label = name,
        .source_location = source_location(node),
        .kind = "import",
        .confidence = Confidence::Extracted,
        // Module + name let a post-merge pass relink this stub onto the real
        // declared symbol in the imported file when that file is in the graph.
        .properties = {{"import_path", module_key}},
    });
    if (!alias.empty() && alias != name) {
      // `import { config as configModule }`: the file's own code says
      // `configModule`. resolve_imports carries this onto the relinked edge so
      // name resolution in this file can bind the alias.
      fragment.nodes.back().properties.emplace("alias", alias);
    }
    fragment.edges.push_back(Edge{
        .source = file_id,
        .target = symbol_id,
        .relation = symbol_relation,
        .confidence = Confidence::Extracted,
    });
  }
}

// A declaration sits at module scope when it is a direct child of the program
// or of a top-level `export`. Mirrors Graphify's `is_module_level` test.
[[nodiscard]] bool is_module_level_declaration(const TSNode& node) {
  const TSNode parent = ts_node_parent(node);
  if (ts_node_is_null(parent)) {
    return false;
  }
  const std::string_view parent_type = ts_node_type(parent);
  if (parent_type == "program") {
    return true;
  }
  if (parent_type == "export_statement") {
    const TSNode grandparent = ts_node_parent(parent);
    return !ts_node_is_null(grandparent) && std::string_view(ts_node_type(grandparent)) == "program";
  }
  return false;
}

// Emits a node for each module-level `const` whose value is an object, array,
// type assertion, or factory/constructor call — e.g. a Zustand store
// (`export const useStore = create(...)`), a context, or a config literal.
// Graphify materialises these as first-class nodes (extract.py module-level
// const handling); they are heavily-referenced hub symbols, so calls to them
// (`useStore()`) resolve to a real target instead of dropping. Arrow-valued
// consts are handled by the function branch of the generic walk, so they are
// skipped here.
// The URL prefix a router chain gives every route registered on it: Elysia's
// `new Elysia({ prefix: '/notebooks' })` option or Hono's `.basePath('/v1')`
// link, read by walking the chain expression down to its constructor. Empty for
// a chain with neither (`express()`, `Router()`, `new Elysia()`).
[[nodiscard]] std::string chain_route_prefix(const TSNode& value, std::string_view source) {
  std::string base_path;
  TSNode current = unwrap_expression(value);
  while (!ts_node_is_null(current)) {
    const std::string_view type = ts_node_type(current);
    if (type == "member_expression") {
      current = unwrap_expression(ts_node_child_by_field_name(current, "object", 6));
      continue;
    }
    if (type == "call_expression") {
      const TSNode callee = ts_node_child_by_field_name(current, "function", 8);
      if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression") {
        break;  // a factory call (`express()`, `Router()`) carries no prefix
      }
      if (field_text(callee, "property", source) == "basePath") {
        const TSNode arguments = ts_node_child_by_field_name(current, "arguments", 9);
        if (!ts_node_is_null(arguments) && ts_node_named_child_count(arguments) > 0) {
          if (const TSNode path = ts_node_named_child(arguments, 0); is_string_value(path)) {
            base_path = strip_string_quotes(node_text(path, source));
          }
        }
      }
      current = unwrap_expression(ts_node_child_by_field_name(callee, "object", 6));
      continue;
    }
    if (type == "new_expression") {
      const TSNode arguments = ts_node_child_by_field_name(current, "arguments", 9);
      if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0) {
        break;
      }
      const TSNode options = ts_node_named_child(arguments, 0);
      if (std::string_view(ts_node_type(options)) != "object") {
        break;
      }
      const auto pair_count = ts_node_named_child_count(options);
      for (std::uint32_t index = 0; index < pair_count; ++index) {
        const TSNode pair = ts_node_named_child(options, index);
        if (std::string_view(ts_node_type(pair)) != "pair") {
          continue;
        }
        if (strip_string_quotes(field_text(pair, "key", source)) != "prefix") {
          continue;
        }
        if (const TSNode prefix = ts_node_child_by_field_name(pair, "value", 5);
            !ts_node_is_null(prefix) && is_string_value(prefix)) {
          return strip_string_quotes(node_text(prefix, source));
        }
      }
      break;
    }
    break;
  }
  return base_path;
}

// `apiRoutes.use(notebookRoutes)`, `app.use('/api', router)`, `app.route('/api',
// sub)`: the chain rooted at a module-level variable mounts the chain the
// identifier names, under the mount path when the framework takes one. The
// target is resolved after merge (it is usually imported), so this records the
// fact; a plugin or middleware argument that is not a bare identifier
// (`.use(cors())`) is not a mount.
void route_mount_handler(const TSNode& node, const ExtractionContext& context, std::vector<RawRelation>& out) {
  if (std::string_view(ts_node_type(node)) != "call_expression") {
    return;
  }
  const TSNode callee = ts_node_child_by_field_name(node, "function", 8);
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "member_expression") {
    return;
  }
  const auto method = field_text(callee, "property", context.source);
  if (method != "use" && method != "route") {
    return;
  }
  const TSNode arguments = ts_node_child_by_field_name(node, "arguments", 9);
  if (ts_node_is_null(arguments)) {
    return;
  }
  const auto argument_count = ts_node_named_child_count(arguments);
  if (argument_count == 0) {
    return;
  }
  std::string prefix;
  TSNode target = ts_node_named_child(arguments, 0);
  if (is_string_value(target)) {
    if (argument_count < 2) {
      return;  // `router.route('/x')` opens an Express route chain, mounts nothing
    }
    prefix = strip_string_quotes(node_text(target, context.source));
    target = ts_node_named_child(arguments, 1);
  }
  target = unwrap_expression(target);
  if (ts_node_is_null(target) || std::string_view(ts_node_type(target)) != "identifier") {
    return;
  }
  const auto chain = resolve_chain(node, context);
  if (chain.root.empty() || !chain.resolvable) {
    return;
  }
  if (!chain.prefix.empty()) {
    prefix = join_route_path(chain.prefix, prefix);  // a mount inside a `.group('/p', …)` callback
  }
  out.push_back(RawRelation{
      .source_id = make_id(context.source_file + ":" + chain.root),
      .target_label = node_text(target, context.source),
      .relation = "mounts",
      .context = std::move(prefix),
      .source_file = context.source_file,
  });
}

void module_const_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment, std::vector<RawRelation>& raw_relations) {
  const std::string_view type = ts_node_type(node);
  if (type != "lexical_declaration" && type != "variable_declaration") {
    return;
  }
  if (!is_module_level_declaration(node)) {
    return;
  }
  const std::string file_id = make_id(context.source_file);
  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const TSNode declarator = ts_node_child(node, index);
    if (std::string_view(ts_node_type(declarator)) != "variable_declarator") {
      continue;
    }
    const TSNode value = ts_node_child_by_field_name(declarator, "value", 5);
    if (ts_node_is_null(value)) {
      continue;
    }
    const std::string_view value_type = ts_node_type(value);
    if (value_type != "object" && value_type != "array" && value_type != "as_expression" &&
        value_type != "call_expression" && value_type != "new_expression") {
      continue;
    }
    auto name = field_text(declarator, "name", context.source);
    if (name.empty()) {
      continue;
    }
    auto id = make_id(context.source_file + ":" + name);
    fragment.nodes.push_back(Node{
        .id = id,
        .label = std::move(name),
        .source_file = context.source_file,
        .source_location = source_location(declarator),
        .kind = "variable",
        .confidence = Confidence::Extracted,
    });
    if (auto prefix = chain_route_prefix(value, context.source); !prefix.empty()) {
      fragment.nodes.back().properties.emplace("route_prefix", std::move(prefix));
    }
    // `export const deckModule: Elysia = deckRoutes as unknown as Elysia;` is the
    // same chain under a second name (turing-api type-collapses modules this
    // way). resolve_contracts treats the alias as a mount with no path.
    if (const TSNode aliased = unwrap_expression(value);
        !ts_node_is_null(aliased) && std::string_view(ts_node_type(aliased)) == "identifier") {
      raw_relations.push_back(RawRelation{
          .source_id = id,
          .target_label = node_text(aliased, context.source),
          .relation = "aliases",
          .source_file = context.source_file,
      });
    }
    fragment.edges.push_back(Edge{
        .source = file_id,
        .target = std::move(id),
        .relation = "contains",
        .confidence = Confidence::Extracted,
    });
  }
}

// ---- HTTP consumers (contracts.hpp: http_call / http_wrapper) -------------

// The value of a module-level `const NAME = ...` in this file (through casts),
// or null. Base URLs are spelled this way (`const base = \`${API_URL}/api/v1\``)
// and a wrapper's template inlines them.
[[nodiscard]] TSNode module_const_value(const TSNode& from, std::string_view name, std::string_view source) {
  TSNode program = from;
  for (TSNode parent = ts_node_parent(program); !ts_node_is_null(parent); parent = ts_node_parent(program)) {
    program = parent;
  }
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

// A URL argument reduced to the path it names. Literal text is kept; an
// interpolation at the start is the host and is dropped; one that fills a whole
// segment is a parameter, `{}`; the enclosing function's first parameter at the
// end is the tail a wrapper appends its argument to; anything else mid-segment
// makes the URL unresolvable. The query string and fragment are not part of the
// route. A literal absolute URL names another service and is unresolvable here.
struct UrlTemplate {
  std::string path;
  bool tail = false;
  bool resolvable = true;
  bool in_query = false;
  bool dropped_host = false;

  void literal(std::string_view text) {
    if (in_query) {
      return;
    }
    if (tail && !text.empty()) {
      resolvable = false;  // text after the appended argument: not a prefix wrapper
      return;
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
  // The enclosing function's first parameter interpolated into the URL. After a
  // slash it fills a segment like any other value (`/projects/${projectId}/publish`
  // in `publishProject(projectId)`); appended to text (`${base}${path}`) or
  // standing alone (`fetch(url)`) it is the tail a wrapper forwards.
  void parameter(std::string_view /*name*/) {
    if (in_query) {
      return;
    }
    if (!path.empty() && path.back() == '/') {
      path += "{}";
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
  }
  // An identifier this file does not define, in the base position: kept as a
  // `${NAME}` placeholder for resolve_contracts, which inlines the constant when
  // exactly one file in the project defines a URL constant of that name.
  void reference(std::string_view name) {
    if (in_query) {
      return;
    }
    if (!path.empty()) {
      unknown(true);
      return;
    }
    path = "${" + std::string(name) + "}";
  }
  void finish() {
    if (path.starts_with("http://") || path.starts_with("https://")) {
      resolvable = false;  // another host, spelled out: not this repository's contract
      return;
    }
    if (!tail && (path.empty() || (path.front() != '/' && !path.starts_with("${")))) {
      resolvable = false;
    }
  }
};

constexpr int kMaxUrlInlineDepth = 3;

void collect_url_template(const TSNode& node, const ExtractionContext& context, std::string_view tail_parameter,
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
        collect_url_template(ts_node_named_child(part, 0), context, tail_parameter, url, depth);
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
    collect_url_template(ts_node_child_by_field_name(expression, "left", 4), context, tail_parameter, url, depth);
    collect_url_template(ts_node_child_by_field_name(expression, "right", 5), context, tail_parameter, url, depth);
    return;
  }
  if (type == "identifier") {
    const auto name = node_text(expression, context.source);
    if (!tail_parameter.empty() && name == tail_parameter) {
      url.parameter(name);
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
    // A variable declared in an enclosing function body (`const url = build();
    // fetch(url)`) is local: no constant anywhere can stand for it.
    for (TSNode ancestor = ts_node_parent(expression); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
      if (is_function_node(ts_node_type(ancestor)) &&
          declares_local(ts_node_child_by_field_name(ancestor, "body", 4), name, context.source)) {
        url.unknown(true);
        return;
      }
    }
    url.reference(name);  // imported or otherwise unknown: resolved project-wide, or refused
    return;
  }
  if (type == "member_expression" && node_text(expression, context.source).starts_with("process.env.")) {
    url.unknown(false);
    return;
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
      return make_id(context.source_file + ":" + field_text(ancestor, "name", context.source));
    }
  }
  return make_id(context.source_file);
}

// The `method: 'POST'` of an options object literal, uppercased; empty when the
// options are absent, not a literal, or carry no literal method.
[[nodiscard]] std::string options_method(const TSNode& options, std::string_view source) {
  const TSNode object = unwrap_expression(options);
  if (ts_node_is_null(object) || std::string_view(ts_node_type(object)) != "object") {
    return {};
  }
  const auto count = ts_node_named_child_count(object);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode pair = ts_node_named_child(object, index);
    if (std::string_view(ts_node_type(pair)) != "pair" || strip_string_quotes(field_text(pair, "key", source)) != "method") {
      continue;
    }
    const TSNode value = ts_node_child_by_field_name(pair, "value", 5);
    if (ts_node_is_null(value) || !is_string_value(value)) {
      return {};
    }
    std::string method = strip_string_quotes(node_text(value, source));
    for (auto& ch : method) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return method;
  }
  return {};
}

// `fetch(url, opts)`, `api.GET('/path')`, `axios.post(url)` and calls to a
// wrapper with a path-like first argument record `http_call` facts; a function
// whose own client call appends its first parameter to a fixed prefix records an
// `http_wrapper` fact instead of a call of its own.
void http_call_handler(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                       std::vector<RawRelation>& out) {
  if (std::string_view(ts_node_type(node)) != "call_expression") {
    return;
  }
  const TSNode callee = unwrap_expression(ts_node_child_by_field_name(node, "function", 8));
  if (ts_node_is_null(callee)) {
    return;
  }
  std::string client;
  std::string verb;
  const std::string_view callee_type = ts_node_type(callee);
  if (callee_type == "identifier") {
    client = node_text(callee, context.source);
  } else if (callee_type == "member_expression") {
    const auto property = field_text(callee, "property", context.source);
    std::string lower(property);
    for (auto& ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (!is_http_verb(lower) || lower == "all") {
      return;
    }
    const TSNode receiver = unwrap_expression(ts_node_child_by_field_name(callee, "object", 6));
    if (ts_node_is_null(receiver)) {
      return;
    }
    const std::string_view receiver_type = ts_node_type(receiver);
    const auto receiver_name = receiver_type == "identifier"
                                   ? node_text(receiver, context.source)
                                   : receiver_type == "member_expression" ? field_text(receiver, "property", context.source)
                                                                          : std::string{};
    if (!looks_like_http_client(receiver_name)) {
      return;
    }
    client = receiver_name + "." + property;
    verb = lower;
  } else {
    return;
  }
  const TSNode arguments = ts_node_child_by_field_name(node, "arguments", 9);
  if (ts_node_is_null(arguments)) {
    return;
  }
  const auto argument_count = ts_node_named_child_count(arguments);
  if (argument_count == 0) {
    return;
  }
  for (std::uint32_t index = 0; index < argument_count; ++index) {
    if (is_function_value(ts_node_named_child(arguments, index))) {
      return;  // a handler argument: this is a route registration (or a callback API), not a request
    }
  }
  const TSNode first = ts_node_named_child(arguments, 0);
  const std::string_view first_type = ts_node_type(unwrap_expression(first));
  if (first_type != "string" && first_type != "template_string" && first_type != "identifier" &&
      first_type != "binary_expression") {
    return;
  }
  const bool primitive = client == "fetch" || !verb.empty();
  // The first parameter of the function the call sits in: a wrapper appends it.
  std::string tail_parameter;
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    if (is_function_node(ts_node_type(ancestor))) {
      std::vector<std::string> parameters;
      parameter_names(ancestor, context.source, parameters);
      if (!parameters.empty()) {
        tail_parameter = parameters.front();
      }
      break;
    }
  }
  UrlTemplate url;
  collect_url_template(first, context, tail_parameter, url, 0);
  url.finish();
  const auto method = argument_count >= 2 ? options_method(ts_node_named_child(arguments, 1), context.source)
                                          : std::string{};
  if (url.tail) {
    // This call appends the enclosing function's first parameter: the function
    // is a wrapper, and callers of it are the consumers.
    if (primitive && url.resolvable && !function_scope_id.empty()) {
      out.push_back(RawRelation{
          .source_id = function_scope_id,
          .target_label = client,
          .relation = "http_wrapper",
          .context = method + " " + url.path,
          .source_file = context.source_file,
      });
    }
    return;
  }
  if (!primitive && (!url.resolvable || url.path.empty())) {
    return;  // a function taking some string: only a path-like literal marks a wrapper call
  }
  out.push_back(RawRelation{
      .source_id = consumer_scope_id(node, context, function_scope_id),
      .target_label = std::move(client),
      .relation = "http_call",
      .context = method + " " + (url.resolvable ? url.path : std::string{}),
      .source_file = context.source_file,
  });
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
      if (url.resolvable && !url.tail) {
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
        .source_id = make_id(context.source_file),
        .target_label = field_text(declarator, "name", context.source),
        .relation = "url_const",
        .context = std::move(path),
        .source_file = context.source_file,
    });
  }
}

void js_extra_walk(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                   Fragment& fragment, std::vector<RawCall>& /*raw_calls*/, std::vector<RawRelation>& raw_relations) {
  module_const_handler(node, context, fragment, raw_relations);
  route_mount_handler(node, context, raw_relations);
  url_const_handler(node, context, raw_relations);
  http_call_handler(node, context, function_scope_id, raw_relations);
}

// TS primitive/builtin type names that never become a `references` target.
[[nodiscard]] bool is_primitive_type(std::string_view name) {
  static const std::unordered_set<std::string_view> primitives = {
      "string", "number", "boolean", "any", "unknown", "void", "never",
      "object", "null", "undefined", "bigint", "symbol", "this",
  };
  return primitives.contains(name);
}

// Reduces a (possibly dotted) type name to its tail: `React.FC` -> `FC`.
[[nodiscard]] std::string type_name_tail(std::string text) {
  if (const auto dot = text.rfind('.'); dot != std::string::npos) {
    return text.substr(dot + 1);
  }
  return text;
}

// Walks a TS type-annotation subtree, appending (name, is_generic_arg) for every
// referenced type, skipping primitives. Mirrors Graphify's _ts_collect_type_refs:
// `type_annotation` recurses; identifiers are leaves; `generic_type` yields its
// base name and recurses into `type_arguments` with the generic-arg role.
void collect_type_refs(const TSNode& node, std::string_view source, bool generic, std::vector<std::pair<std::string, bool>>& out) {
  if (ts_node_is_null(node)) {
    return;
  }
  const std::string_view type = ts_node_type(node);
  const auto emit = [&](std::string text) {
    text = type_name_tail(std::move(text));
    if (!text.empty() && !is_primitive_type(text)) {
      out.emplace_back(std::move(text), generic);
    }
  };
  if (type == "type_annotation") {
    const auto count = ts_node_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto child = ts_node_child(node, index);
      if (ts_node_is_named(child)) {
        collect_type_refs(child, source, generic, out);
      }
    }
    return;
  }
  if (type == "type_identifier" || type == "identifier" || type == "nested_type_identifier") {
    emit(node_text(node, source));
    return;
  }
  if (type == "generic_type") {
    if (const auto name = ts_node_child_by_field_name(node, "name", 4); !ts_node_is_null(name)) {
      emit(node_text(name, source));
    }
    const auto count = ts_node_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto child = ts_node_child(node, index);
      if (std::string_view(ts_node_type(child)) != "type_arguments") {
        continue;
      }
      const auto arg_count = ts_node_child_count(child);
      for (std::uint32_t arg = 0; arg < arg_count; ++arg) {
        const auto sub = ts_node_child(child, arg);
        if (ts_node_is_named(sub)) {
          collect_type_refs(sub, source, true, out);
        }
      }
    }
    return;
  }
  if (ts_node_is_named(node)) {
    const auto count = ts_node_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto child = ts_node_child(node, index);
      if (ts_node_is_named(child)) {
        collect_type_refs(child, source, generic, out);
      }
    }
  }
}

// Appends the base type names of a heritage clause (`extends A`, `implements B,
// C`, interface `extends D`). Handles identifier/type_identifier, generic_type
// (via its `name` field), and nested_type_identifier — each reduced to its tail.
void collect_heritage_names(const TSNode& clause, std::string_view source, std::vector<std::string>& out) {
  const auto count = ts_node_child_count(clause);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto child = ts_node_child(clause, index);
    if (!ts_node_is_named(child)) {
      continue;
    }
    const std::string_view type = ts_node_type(child);
    if (type == "identifier" || type == "type_identifier" || type == "nested_type_identifier") {
      if (auto name = type_name_tail(node_text(child, source)); !name.empty()) {
        out.push_back(std::move(name));
      }
    } else if (type == "generic_type") {
      if (const auto name = ts_node_child_by_field_name(child, "name", 4); !ts_node_is_null(name)) {
        if (auto text = type_name_tail(node_text(name, source)); !text.empty()) {
          out.push_back(std::move(text));
        }
      }
    }
  }
}

// Emits inherits/implements (from a class/interface heritage clause) and
// references (from member parameter/return/field type annotations) facts. The
// node id is the class or interface node; method references are sourced from the
// method node id, built with the same scheme the generic walk uses
// (`make_id(source_file + ":" + method_name)`) so they land on a real node.
// HTTP contract facts for resolve_contracts (contracts.hpp). An inline route
// handler records the chain it is registered on and the route as written; an
// exported `GET`/`POST`/... at the top of a Next.js `app/**/route.ts` records
// the path its file serves, with no chain. The endpoint node itself is minted
// after merge, once the chain's mounts across files are known.
void push_route_facts(const TSNode& node, const ExtractionContext& context, const std::string& node_id, std::vector<RawRelation>& out) {
  if (const auto registration = route_registration(node, context)) {
    // An unresolvable chain leaves the target empty: resolve_contracts counts
    // it rather than guessing a path.
    out.push_back(RawRelation{
        .source_id = node_id,
        .target_label = registration->resolvable ? registration->root : std::string{},
        .relation = "route",
        .context = registration->verb + " " + registration->path,
        .source_file = context.source_file,
    });
    return;
  }
  const auto file_path = next_route_path(context.source_file);
  if (!file_path) {
    return;
  }
  std::string name;
  const std::string_view type = ts_node_type(node);
  if (type == "function_declaration") {
    if (!is_module_level_declaration(node)) {
      return;
    }
    name = field_text(node, "name", context.source);
  } else if (is_function_value(node)) {
    const TSNode declarator = ts_node_parent(node);
    if (ts_node_is_null(declarator) || std::string_view(ts_node_type(declarator)) != "variable_declarator") {
      return;
    }
    const TSNode declaration = ts_node_parent(declarator);
    if (ts_node_is_null(declaration) || !is_module_level_declaration(declaration)) {
      return;
    }
    name = field_text(declarator, "name", context.source);
  } else {
    return;
  }
  // Next.js exports the verb in capitals; a lowercase `get` in a route file is
  // an ordinary helper.
  std::string verb(name);
  for (auto& ch : verb) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (verb.empty() || verb == name || !is_http_verb(verb)) {
    return;
  }
  out.push_back(RawRelation{
      .source_id = node_id,
      .target_label = {},
      .relation = "file_route",  // the path is the file's: absolute, no chain to compose
      .context = verb + " " + *file_path,
      .source_file = context.source_file,
  });
}

void ts_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id, std::vector<RawRelation>& out) {
  const std::string_view node_type = ts_node_type(node);
  const bool is_class = node_type == "class_declaration" || node_type == "abstract_class_declaration";
  const bool is_interface = node_type == "interface_declaration";
  if (!is_class && !is_interface) {
    push_route_facts(node, context, node_id, out);
    return;  // type aliases and enums have no heritage or members
  }

  const auto push_heritage = [&](const TSNode& clause, std::string relation) {
    std::vector<std::string> names;
    collect_heritage_names(clause, context.source, names);
    for (auto& name : names) {
      out.push_back(RawRelation{
          .source_id = node_id,
          .target_label = std::move(name),
          .relation = relation,
          .context = "type",
          .source_file = context.source_file,
          .allow_same_file = true,  // `class A extends B` may resolve to a same-file B
      });
    }
  };

  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const auto child = ts_node_child(node, index);
    const std::string_view child_type = ts_node_type(child);
    if (child_type == "class_heritage") {
      const auto clause_count = ts_node_child_count(child);
      for (std::uint32_t clause_index = 0; clause_index < clause_count; ++clause_index) {
        const auto clause = ts_node_child(child, clause_index);
        if (std::string_view(ts_node_type(clause)) == "extends_clause") {
          push_heritage(clause, "inherits");
        } else if (std::string_view(ts_node_type(clause)) == "implements_clause") {
          push_heritage(clause, "implements");
        }
      }
    } else if (child_type == "extends_type_clause") {
      push_heritage(child, "inherits");  // interface inheritance
    }
  }

  const auto body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(body)) {
    return;
  }
  const auto emit_refs = [&](const TSNode& type_node, const std::string& source_id, const char* base_context) {
    std::vector<std::pair<std::string, bool>> refs;
    collect_type_refs(type_node, context.source, false, refs);
    for (auto& [name, is_generic] : refs) {
      out.push_back(RawRelation{
          .source_id = source_id,
          .target_label = std::move(name),
          .relation = "references",
          .context = is_generic ? "generic_arg" : base_context,
          .source_file = context.source_file,
          .allow_same_file = false,  // Graphify resolves references only via imports
      });
    }
  };

  const auto member_count = ts_node_child_count(body);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    const auto member = ts_node_child(body, index);
    const std::string_view member_type = ts_node_type(member);
    if (member_type == "method_definition" || member_type == "method_signature" || member_type == "abstract_method_signature") {
      const auto name_node = ts_node_child_by_field_name(member, "name", 4);
      if (ts_node_is_null(name_node)) {
        continue;
      }
      const auto method_id = make_id(context.source_file + ":" + node_text(name_node, context.source));
      if (const auto params = ts_node_child_by_field_name(member, "parameters", 10); !ts_node_is_null(params)) {
        const auto param_count = ts_node_child_count(params);
        for (std::uint32_t param = 0; param < param_count; ++param) {
          const auto parameter = ts_node_child(params, param);
          const std::string_view parameter_type = ts_node_type(parameter);
          if (parameter_type != "required_parameter" && parameter_type != "optional_parameter") {
            continue;
          }
          if (const auto annotation = ts_node_child_by_field_name(parameter, "type", 4); !ts_node_is_null(annotation)) {
            emit_refs(annotation, method_id, "parameter_type");
          }
        }
      }
      if (const auto return_type = ts_node_child_by_field_name(member, "return_type", 11); !ts_node_is_null(return_type)) {
        emit_refs(return_type, method_id, "return_type");
      }
    } else if (member_type == "public_field_definition" || member_type == "property_signature") {
      const auto field_count = ts_node_child_count(member);
      for (std::uint32_t field = 0; field < field_count; ++field) {
        const auto child = ts_node_child(member, field);
        if (std::string_view(ts_node_type(child)) == "type_annotation") {
          emit_refs(child, node_id, "field");
          break;
        }
      }
    }
  }
}

// `constructor(public readonly x: number)` declares a member, the same way a
// Java record component does (java_member_handler). The grammar wraps the
// parameter in `required_parameter`/`optional_parameter` with an accessibility
// modifier; without one it is an ordinary parameter and declares nothing.
void ts_parameter_properties(const TSNode& method, const ExtractionContext& context,
                             const std::string& owner_id, const std::string& owner_name,
                             Fragment& fragment) {
  if (field_text(method, "name", context.source) != "constructor") return;
  const auto parameters = ts_node_child_by_field_name(method, "parameters", 10);
  if (ts_node_is_null(parameters)) return;
  for (std::uint32_t i = 0; i < ts_node_named_child_count(parameters); ++i) {
    const auto parameter = ts_node_named_child(parameters, i);
    const std::string_view kind = ts_node_type(parameter);
    if (kind != "required_parameter" && kind != "optional_parameter") continue;
    bool declared = false;
    bool readonly = false;
    for (std::uint32_t j = 0; j < ts_node_child_count(parameter); ++j) {
      const auto child = ts_node_child(parameter, j);
      const std::string_view token = ts_node_type(child);
      declared = declared || token == "accessibility_modifier";
      readonly = readonly || token == "readonly";
    }
    if (!declared) continue;
    auto name = field_text(parameter, "pattern", context.source);
    if (name.empty()) continue;
    const auto annotation = ts_node_child_by_field_name(parameter, "type", 4);
    Properties properties;
    if (!ts_node_is_null(annotation) && ts_node_named_child_count(annotation) > 0) {
      properties.emplace("type_text", node_text(ts_node_named_child(annotation, 0), context.source));
    }
    properties.emplace("optional", kind == "optional_parameter" ? "true" : "false");
    properties.emplace("readonly", readonly ? "true" : "false");
    add_field_node(context, owner_id, owner_name, std::move(name), source_location(parameter),
                   std::move(properties), fragment);
  }
}

// ---- openapi-typescript (contract_schemas.hpp, the TypeScript form) --------
//
// `openapi-typescript` renders an OpenAPI document as `export interface paths`
// (a property per path, a property per method whose type is `never` when the
// method is absent), `export interface operations` (the request and response
// shapes, keyed by operation id) and `export interface components` (`schemas`).
// This is the contract artifact a TypeScript client is typed against, so its
// paths are documented endpoints and its component schemas are `schema` nodes,
// exactly as an OpenAPI JSON document's would be. The 455-member `paths`
// interface no longer yields 455 `field` nodes.

[[nodiscard]] bool is_openapi_typescript(std::string_view source) {
  return source.find("export interface paths") != std::string_view::npos &&
         source.find("export interface operations") != std::string_view::npos;
}

// The interface declaration named `name` at the top of the file, or null.
[[nodiscard]] TSNode interface_named(const TSNode& from, std::string_view name, std::string_view source) {
  TSNode program = from;
  for (TSNode parent = ts_node_parent(program); !ts_node_is_null(parent); parent = ts_node_parent(program)) {
    program = parent;
  }
  const auto count = ts_node_named_child_count(program);
  for (std::uint32_t index = 0; index < count; ++index) {
    TSNode statement = ts_node_named_child(program, index);
    if (std::string_view(ts_node_type(statement)) == "export_statement") {
      statement = ts_node_child_by_field_name(statement, "declaration", 11);
      if (ts_node_is_null(statement)) {
        continue;
      }
    }
    if (std::string_view(ts_node_type(statement)) == "interface_declaration" &&
        field_text(statement, "name", source) == name) {
      return statement;
    }
  }
  return TSNode{};
}

// The type a property signature declares, unwrapped from its annotation.
[[nodiscard]] TSNode property_type(const TSNode& member) {
  if (ts_node_is_null(member)) {
    return TSNode{};  // a member that was not found: tree-sitter dereferences a null node's tree
  }
  const TSNode annotation = ts_node_child_by_field_name(member, "type", 4);
  if (ts_node_is_null(annotation) || ts_node_named_child_count(annotation) == 0) {
    return TSNode{};
  }
  return ts_node_named_child(annotation, 0);
}

// The property signature named `key` (quotes stripped) directly in an object
// type or interface body, or null.
[[nodiscard]] TSNode member_named(const TSNode& body, std::string_view key, std::string_view source) {
  if (ts_node_is_null(body)) {
    return TSNode{};
  }
  const auto count = ts_node_named_child_count(body);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(body, index);
    if (std::string_view(ts_node_type(member)) == "property_signature" &&
        strip_string_quotes(field_text(member, "name", source)) == key) {
      return member;
    }
  }
  return TSNode{};
}

// `components["schemas"]["Notebook"]`, possibly followed by `[]`: the schema name.
[[nodiscard]] std::string components_schema_name(std::string_view type_text) {
  static constexpr std::string_view kPrefix = "components[\"schemas\"][\"";
  if (!type_text.starts_with(kPrefix)) {
    return {};
  }
  const auto end = type_text.find("\"]", kPrefix.size());
  return end == std::string_view::npos ? std::string{} : std::string(type_text.substr(kPrefix.size(), end - kPrefix.size()));
}

// Schema names an operation's 2xx responses and request body refer to.
void operation_schema_refs(const TSNode& operation_type, std::string_view source, std::vector<std::string>& responds,
                           std::vector<std::string>& accepts) {
  if (ts_node_is_null(operation_type) || std::string_view(ts_node_type(operation_type)) != "object_type") {
    return;
  }
  const auto media_refs = [&](const TSNode& holder, std::vector<std::string>& out) {
    const TSNode content = property_type(member_named(holder, "content", source));
    if (ts_node_is_null(content)) {
      return;
    }
    const auto count = ts_node_named_child_count(content);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode media = ts_node_named_child(content, index);
      if (std::string_view(ts_node_type(media)) != "property_signature") {
        continue;
      }
      if (const TSNode type = property_type(media); !ts_node_is_null(type)) {
        if (auto name = components_schema_name(node_text(type, source)); !name.empty()) {
          out.push_back(std::move(name));
        }
      }
    }
  };
  if (const TSNode responses = property_type(member_named(operation_type, "responses", source)); !ts_node_is_null(responses)) {
    const auto count = ts_node_named_child_count(responses);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode response = ts_node_named_child(responses, index);
      if (std::string_view(ts_node_type(response)) != "property_signature") {
        continue;
      }
      const auto status = strip_string_quotes(field_text(response, "name", source));
      if (status.starts_with('2') || status == "default") {
        media_refs(property_type(response), responds);
      }
    }
  }
  media_refs(property_type(member_named(operation_type, "requestBody", source)), accepts);
}

void openapi_typescript_paths(const TSNode& node, const ExtractionContext& context, const std::string& owner_id,
                              Fragment& fragment) {
  const TSNode body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(body)) {
    return;
  }
  const std::string file_id = make_id(context.source_file);
  // Schema references resolve only when the file declares component schemas.
  const TSNode components = interface_named(node, "components", context.source);
  const TSNode schemas_type =
      ts_node_is_null(components) ? TSNode{}
                                  : property_type(member_named(ts_node_child_by_field_name(components, "body", 4), "schemas", context.source));
  const bool has_schemas = !ts_node_is_null(schemas_type) && std::string_view(ts_node_type(schemas_type)) == "object_type";
  const TSNode operations = interface_named(node, "operations", context.source);
  const TSNode operations_body = ts_node_is_null(operations) ? TSNode{} : ts_node_child_by_field_name(operations, "body", 4);

  std::unordered_set<std::string> seen_edges;
  const auto add_edge = [&](const std::string& source, const std::string& target, const char* relation) {
    if (seen_edges.insert(source + '\n' + relation + '\n' + target).second) {
      fragment.edges.push_back(Edge{.source = source, .target = target, .relation = relation, .confidence = Confidence::Extracted});
    }
  };

  const auto path_count = ts_node_named_child_count(body);
  for (std::uint32_t index = 0; index < path_count; ++index) {
    const TSNode path_member = ts_node_named_child(body, index);
    if (std::string_view(ts_node_type(path_member)) != "property_signature") {
      continue;
    }
    const auto raw_path = field_text(path_member, "name", context.source);
    const TSNode name_node = ts_node_child_by_field_name(path_member, "name", 4);
    if (ts_node_is_null(name_node) || !is_string_value(name_node)) {
      continue;
    }
    const auto path = strip_string_quotes(raw_path);
    const TSNode item = property_type(path_member);
    if (ts_node_is_null(item) || std::string_view(ts_node_type(item)) != "object_type") {
      continue;
    }
    const auto method_count = ts_node_named_child_count(item);
    for (std::uint32_t m = 0; m < method_count; ++m) {
      const TSNode method_member = ts_node_named_child(item, m);
      if (std::string_view(ts_node_type(method_member)) != "property_signature") {
        continue;
      }
      const auto verb = field_text(method_member, "name", context.source);
      if (!is_http_verb(verb) || verb == "all") {
        continue;
      }
      const TSNode type = property_type(method_member);
      if (ts_node_is_null(type) || node_text(type, context.source) == "never") {
        continue;  // `post?: never`: the method is not offered
      }
      std::string method(verb);
      for (auto& ch : method) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
      const auto id = "endpoint:" + method + " " + canonical_route_path(path);
      const auto type_text = node_text(type, context.source);
      std::string operation;
      if (type_text.starts_with("operations[\"")) {
        const auto end = type_text.find("\"]", 12);
        operation = end == std::string::npos ? std::string{} : type_text.substr(12, end - 12);
      }
      Properties properties{{"method", method}, {"path", path}, {"documented", "true"}, {"format", "openapi-typescript"}};
      if (!operation.empty()) {
        properties.emplace("operation", operation);
      }
      fragment.nodes.push_back(Node{
          .id = id,
          .label = method + " " + path,
          .source_file = context.source_file,
          .source_location = source_location(method_member),
          .kind = "endpoint",
          .confidence = Confidence::Extracted,
          .properties = std::move(properties),
      });
      add_edge(file_id, id, "contains");
      add_edge(owner_id, id, "defines");
      if (has_schemas && !operation.empty() && !ts_node_is_null(operations_body)) {
        std::vector<std::string> responds;
        std::vector<std::string> accepts;
        operation_schema_refs(property_type(member_named(operations_body, operation, context.source)), context.source,
                              responds, accepts);
        for (const auto& name : responds) {
          add_edge(id, make_id(context.source_file + ":schema:" + name), "RESPONDS_WITH");
        }
        for (const auto& name : accepts) {
          add_edge(id, make_id(context.source_file + ":schema:" + name), "ACCEPTS");
        }
      }
    }
  }
}

void openapi_typescript_components(const TSNode& node, const ExtractionContext& context, const std::string& owner_id,
                                   Fragment& fragment) {
  const TSNode schemas = property_type(member_named(ts_node_child_by_field_name(node, "body", 4), "schemas", context.source));
  if (ts_node_is_null(schemas) || std::string_view(ts_node_type(schemas)) != "object_type") {
    return;  // `schemas: never`: every shape is inlined
  }
  const std::string file_id = make_id(context.source_file);
  std::unordered_set<std::string> names;
  const auto count = ts_node_named_child_count(schemas);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(schemas, index);
    if (std::string_view(ts_node_type(member)) == "property_signature") {
      names.insert(strip_string_quotes(field_text(member, "name", context.source)));
    }
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(schemas, index);
    if (std::string_view(ts_node_type(member)) != "property_signature") {
      continue;
    }
    const auto name = strip_string_quotes(field_text(member, "name", context.source));
    const auto id = make_id(context.source_file + ":schema:" + name);
    fragment.nodes.push_back(Node{
        .id = id,
        .label = name,
        .source_file = context.source_file,
        .source_location = source_location(member),
        .kind = "schema",
        .confidence = Confidence::Extracted,
        .properties = {{"format", "openapi-typescript"}},
    });
    fragment.edges.push_back(Edge{.source = file_id, .target = id, .relation = "contains", .confidence = Confidence::Extracted});
    fragment.edges.push_back(Edge{.source = owner_id, .target = id, .relation = "defines", .confidence = Confidence::Extracted});
    const TSNode shape = property_type(member);
    if (ts_node_is_null(shape) || std::string_view(ts_node_type(shape)) != "object_type") {
      continue;
    }
    std::unordered_set<std::string> referenced;
    const auto field_count = ts_node_named_child_count(shape);
    for (std::uint32_t f = 0; f < field_count; ++f) {
      const TSNode field = ts_node_named_child(shape, f);
      if (std::string_view(ts_node_type(field)) != "property_signature") {
        continue;
      }
      const auto field_name = strip_string_quotes(field_text(field, "name", context.source));
      Properties properties;
      const TSNode type = property_type(field);
      if (!ts_node_is_null(type)) {
        auto type_text = node_text(type, context.source);
        if (const auto target = components_schema_name(type_text); !target.empty() && names.contains(target) && target != name) {
          referenced.insert(target);
        }
        properties.emplace("type_text", std::move(type_text));
      }
      bool optional = false;
      for (std::uint32_t j = 0; j < ts_node_child_count(field); ++j) {
        optional = optional || std::string_view(ts_node_type(ts_node_child(field, j))) == "?";
      }
      properties.emplace("optional", optional ? "true" : "false");
      add_field_node(context, id, name, field_name, source_location(field), std::move(properties), fragment);
    }
    for (const auto& target : referenced) {
      fragment.edges.push_back(Edge{.source = id,
                                    .target = make_id(context.source_file + ":schema:" + target),
                                    .relation = "references",
                                    .confidence = Confidence::Extracted});
    }
  }
}

void ts_member_handler(const TSNode& node, const ExtractionContext& context,
                       const std::string& owner_id, Fragment& fragment) {
  const auto owner_name = field_text(node, "name", context.source);
  if (std::string_view(ts_node_type(node)) == "interface_declaration" && is_openapi_typescript(context.source)) {
    if (owner_name == "paths") {
      openapi_typescript_paths(node, context, owner_id, fragment);
      return;
    }
    if (owner_name == "components") {
      openapi_typescript_components(node, context, owner_id, fragment);
      return;
    }
    if (owner_name == "operations" || owner_name == "webhooks") {
      return;  // request/response shapes are read through the endpoints, not as fields
    }
  }
  auto body = ts_node_child_by_field_name(node, "body", 4);
  if (std::string_view(ts_node_type(node)) == "type_alias_declaration") {
    body = ts_node_child_by_field_name(node, "value", 5);
    if (ts_node_is_null(body) || std::string_view(ts_node_type(body)) != "object_type") return;
  }
  if (owner_name.empty() || ts_node_is_null(body)) return;
  const bool is_enum = std::string_view(ts_node_type(node)) == "enum_declaration";
  for (std::uint32_t i = 0; i < ts_node_named_child_count(body); ++i) {
    const auto member = ts_node_named_child(body, i);
    const std::string_view kind = ts_node_type(member);
    // `method_signature` and `abstract_method_signature` are function nodes
    // already (typescript_language_config below), and a member node would reuse
    // their id and overwrite them.
    if (kind == "method_definition") {
      ts_parameter_properties(member, context, owner_id, owner_name, fragment);
      continue;
    }
    if (!is_enum && kind != "property_signature" && kind != "public_field_definition") continue;
    auto name = field_text(member, "name", context.source);
    if (is_enum && (kind == "property_identifier" || kind == "string" || kind == "number" ||
                    kind == "computed_property_name" || kind == "private_property_identifier")) {
      name = node_text(member, context.source);
    }
    if (name.empty()) continue;
    auto annotation = ts_node_child_by_field_name(member, "type", 4);
    std::string type_text;
    if (!ts_node_is_null(annotation) && ts_node_named_child_count(annotation) > 0) {
      type_text = node_text(ts_node_named_child(annotation, 0), context.source);
    }
    Properties properties;
    if (!type_text.empty()) properties.emplace("type_text", type_text);
    if (!is_enum) {
      bool optional = false;
      bool readonly = false;
      for (std::uint32_t j = 0; j < ts_node_child_count(member); ++j) {
        const std::string_view token = ts_node_type(ts_node_child(member, j));
        optional = optional || token == "?";
        readonly = readonly || token == "readonly";
      }
      properties.emplace("optional", optional ? "true" : "false");
      properties.emplace("readonly", readonly ? "true" : "false");
    }
    add_field_node(context, owner_id, owner_name, std::move(name), source_location(member),
                   std::move(properties), fragment);
  }
}

[[nodiscard]] LanguageConfig base_config(std::string name, std::string grammar, std::vector<std::string> extensions) {
  return LanguageConfig{
      .name = std::move(name),
      .grammar_name = std::move(grammar),
      .extensions = std::move(extensions),
      .class_node_types = {"class_declaration"},
      .function_node_types = {
          "function_declaration",
          "method_definition",
          "generator_function_declaration",
          "arrow_function",
      },
      .import_node_types = {"import_statement", "import_declaration", "export_statement"},
      .call_node_types = {"call_expression", "new_expression"},
      .name_fields = {"name", "property"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function", "constructor"},
      .call_member_node_types = {"member_expression"},
      .call_member_field = "property",
      .import_handler = module_import_handler,
      .resolve_function_name = resolve_js_function_name,
      .extra_walk = js_extra_walk,
      .relation_handler = ts_relation_handler,
      .nested_function_scope = is_route_handler,
  };
}

}  // namespace

LanguageConfig javascript_language_config() {
  return base_config("javascript", "tree-sitter-javascript", {".js", ".jsx", ".mjs", ".cjs"});
}

LanguageConfig typescript_language_config() {
  auto config = base_config("typescript", "tree-sitter-typescript", {".ts"});
  config.extract_members = true;
  config.member_handler = ts_member_handler;
  config.type_node_types = {"interface_declaration", "type_alias_declaration", "enum_declaration"};
  config.class_node_types.push_back("abstract_class_declaration");
  config.function_node_types.push_back("abstract_method_signature");
  config.function_node_types.push_back("method_signature");
  config.function_node_types.push_back("function_signature");
  config.import_node_types.push_back("import_type");
  config.call_node_types.push_back("instantiation_expression");
  return config;
}

LanguageConfig tsx_language_config() {
  auto config = typescript_language_config();
  config.name = "tsx";
  config.grammar_name = "tree-sitter-tsx";
  config.extensions = {".tsx", ".jsx"};
  return config;
}

ExtractionResult extract_javascript(const ExtractionContext& context) {
  auto config = javascript_language_config();
  intern_node_symbols(config, tree_sitter_javascript());
  return extract_with_config(tree_sitter_javascript(), config, context);
}

ExtractionResult extract_typescript(const ExtractionContext& context) {
  auto config = typescript_language_config();
  intern_node_symbols(config, tree_sitter_typescript());
  return extract_with_config(tree_sitter_typescript(), config, context);
}

ExtractionResult extract_tsx(const ExtractionContext& context) {
  auto config = tsx_language_config();
  intern_node_symbols(config, tree_sitter_tsx());
  return extract_with_config(tree_sitter_tsx(), config, context);
}

}  // namespace cgraph
