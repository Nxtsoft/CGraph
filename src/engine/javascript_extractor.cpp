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
    if (std::string_view(ts_node_type(parameter)) == "comment") {
      continue;
    }
    if (std::string_view(ts_node_type(parameter)) == "identifier") {
      out.push_back(node_text(parameter, source));
      continue;
    }
    TSNode pattern = ts_node_child_by_field_name(parameter, "pattern", 7);
    if (std::string_view(ts_node_type(parameter)) == "assignment_pattern") {
      pattern = ts_node_child_by_field_name(parameter, "left", 4);  // JavaScript `method = 'GET'`
    }
    // A destructured parameter keeps its position with an empty name: callers
    // read later parameters by index (`request({ a }, path)` takes the path at 1).
    out.push_back(!ts_node_is_null(pattern) && std::string_view(ts_node_type(pattern)) == "identifier"
                      ? node_text(pattern, source)
                      : std::string{});
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

// The stub id input for a specifier: a relative specifier is resolved against
// the project-relative directory in the relative namespace
// (relative_import_stub), so it neither carries the checkout's location nor
// normalizes onto a bare package's id; a bare specifier keeps "import-<kind>:".
[[nodiscard]] std::string module_stub(std::string_view kind, const std::string& relative_path, const std::string& spec) {
  if (!spec.empty() && spec.front() == '.') {
    return relative_import_stub(kind, std::filesystem::path(relative_path).parent_path() / spec);
  }
  return "import-" + std::string(kind) + ":" + spec;
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

  const std::string file_id = make_id(context.relative_path);
  const std::string module_relation = is_export ? "re_exports" : "imports_from";
  const std::string symbol_relation = is_export ? "re_exports" : "imports";

  if (has_source) {
    const auto spec = strip_string_quotes(node_text(source, context.source));
    if (!spec.empty()) {
      // import_path is resolved against the absolute source file because
      // resolve_imports looks it up by the file nodes' absolute source paths;
      // the stub's id is resolved against the project-relative path so, like
      // every other id, it never carries the checkout's location.
      const auto resolved = resolve_module_spec(context.source_file, spec);
      // The stub id is namespaced so it can never equal a real node's id. A
      // specifier that spells the source extension ("./chunkBy.ts", legal under
      // allowImportingTsExtensions) resolves to the imported file's exact path,
      // and an un-namespaced make_id(resolved) would collide with that file
      // node's id. Fragments merge in path order and "X.spec.ts" sorts before
      // "X.ts", so the stub claimed the id first, merge_fragment discarded the
      // real file node as a duplicate, and resolve_imports — finding no file
      // node for the path — deleted the stub and every edge with it (issue #39).
      const auto module_id = make_id(module_stub("module", context.relative_path, spec));
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
      // `export ... from` statements. `export * from './x'` (a bare star, not
      // `export * as ns`) re-exports every name x exports, so the edge is marked
      // for resolve_imports to follow when an import of a name lands on this
      // barrel file. The mark is on the per-file edge, not the stub: module
      // stubs are shared by every file importing the same path.
      Edge module_edge{
          .source = file_id,
          .target = module_id,
          .relation = module_relation,
          .confidence = Confidence::Extracted,
      };
      if (is_export) {
        bool star = false;
        bool namespaced = false;
        for (std::uint32_t i = 0; i < ts_node_child_count(node); ++i) {
          const std::string_view type = ts_node_type(ts_node_child(node, i));
          star = star || type == "*";
          namespaced = namespaced || type == "namespace_export";
        }
        if (star && !namespaced) {
          module_edge.properties.emplace("star", "true");
        }
      }
      fragment.edges.push_back(std::move(module_edge));
    }
  }

  // file -> each named/default/namespace symbol. Keyed by module+name so the
  // same symbol imported by many files collapses onto one hub node. As for the
  // module stub, the id key is project-relative while import_path stays
  // absolute: resolve_imports looks files up by their absolute source paths,
  // and a relative import_path would fall through to suffix matching, which
  // drops the import whenever two files share a basename.
  const auto spec = has_source ? strip_string_quotes(node_text(source, context.source)) : std::string{};
  const auto symbol_stub = has_source ? module_stub("symbol", context.relative_path, spec)
                                      : relative_import_stub("symbol", context.relative_path);
  const auto module_path = has_source ? resolve_module_spec(context.source_file, spec) : context.source_file;
  std::vector<std::pair<std::string, std::string>> names;
  collect_specifier_names(node, context.source, names);
  for (auto& [name, alias] : names) {
    // Namespaced for the same reason as module_id above: with an
    // extension-spelled specifier, make_id(module_key + ":" + name) is exactly
    // the id add_symbol_node gives the real declared symbol, and the squatting
    // stub deletes the real function from the graph (issue #39/#40).
    const auto symbol_id = make_id(symbol_stub + ":" + name);
    fragment.nodes.push_back(Node{
        .id = symbol_id,
        .label = name,
        .source_location = source_location(node),
        .kind = "import",
        .confidence = Confidence::Extracted,
        // Module + name let a post-merge pass relink this stub onto the real
        // declared symbol in the imported file when that file is in the graph.
        .properties = {{"import_path", module_path}},
    });
    if (!alias.empty() && alias != name) {
      // `import { config as configModule }`: the file's own code says
      // `configModule`. resolve_imports carries this onto the relinked edge so
      // name resolution in this file can bind the alias.
      fragment.nodes.back().properties.emplace("alias", alias);
    }
    Edge symbol_edge{
        .source = file_id,
        .target = symbol_id,
        .relation = symbol_relation,
        .confidence = Confidence::Extracted,
    };
    if (is_export && has_source) {
      // `export { a as b } from './x'`: this file exports the name `b`, which is
      // x's `a`. The exported name travels on the per-file edge (symbol stubs are
      // shared across importing files) so resolve_imports can follow an import of
      // `b` through this barrel to the declaration.
      symbol_edge.properties.emplace("reexport", alias.empty() ? name : alias);
    }
    fragment.edges.push_back(std::move(symbol_edge));
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
      .source_id = make_id(context.relative_path + ":" + chain.root),
      .target_label = node_text(target, context.source),
      .relation = "mounts",
      .context = std::move(prefix),
      .source_file = context.source_file,
  });
}

// The SQL table name a Drizzle table constructor declares: `competitors` for
// `pgTable('competitors', {...})` (likewise mysqlTable / sqliteTable). Empty for
// any other value, and for a name that is not a plain string literal.
[[nodiscard]] std::string drizzle_table_name(const TSNode& value, std::string_view source) {
  const TSNode call = unwrap_expression(value);
  if (ts_node_is_null(call) || std::string_view(ts_node_type(call)) != "call_expression") {
    return {};
  }
  const TSNode callee = ts_node_child_by_field_name(call, "function", 8);
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "identifier") {
    return {};
  }
  const auto name = node_text(callee, source);
  if (name != "pgTable" && name != "mysqlTable" && name != "sqliteTable") {
    return {};
  }
  const TSNode arguments = ts_node_child_by_field_name(call, "arguments", 9);
  if (ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0) {
    return {};
  }
  const TSNode first = ts_node_named_child(arguments, 0);
  if (std::string_view(ts_node_type(first)) != "string") {
    return {};
  }
  return strip_string_quotes(node_text(first, source));
}

void module_const_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment, std::vector<RawRelation>& raw_relations) {
  const std::string_view type = ts_node_type(node);
  if (type != "lexical_declaration" && type != "variable_declaration") {
    return;
  }
  if (!is_module_level_declaration(node)) {
    return;
  }
  const std::string file_id = make_id(context.relative_path);
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
    auto id = make_id(context.relative_path + ":" + name);
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
    // `export const competitors = pgTable('competitors', {...})` is the ORM model
    // of a SQL table. resolve_contracts links it to the migration's `sql_table`
    // node, so impact from a table reaches the code that uses its model.
    if (auto table = drizzle_table_name(value, context.source); !table.empty()) {
      raw_relations.push_back(RawRelation{
          .source_id = id,
          .target_label = std::move(table),
          .relation = "maps_table",
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
    // A type annotation, a default value or a renamed key names other things.
    const char* field = ts_node_field_name_for_named_child(node, index);
    const std::string_view field_name = field == nullptr ? std::string_view{} : std::string_view(field);
    if (std::string_view(ts_node_type(child)) == "type_annotation" || field_name == "value" || field_name == "right" ||
        field_name == "key" || field_name == "type") {
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
};

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

// Every value the local `name` can hold where `use` reads it: its initializer
// and each plain assignment to it before `use` (`let url; if (a) url = x; else
// url = y;`). Empty optional when `name` is not a local; an empty list when it
// is one whose value cannot be known here: it is updated in place (`url +=`),
// assigned inside another function than the read (a test's `beforeAll`),
// its path is rewritten (`url.pathname = ...`), or it is never given a value.
[[nodiscard]] std::optional<std::vector<TSNode>> local_values(const TSNode& use, std::string_view name, std::string_view source) {
  const auto binding = local_binding(use, name, source);
  if (!binding) {
    return std::nullopt;
  }
  std::vector<TSNode> values;
  if (const TSNode initial = ts_node_child_by_field_name(binding->declarator, "value", 5); !ts_node_is_null(initial)) {
    values.push_back(unwrap_expression(initial));
  }
  bool unknowable = false;
  const auto use_start = ts_node_start_byte(use);
  const TSNode use_function = enclosing_function(use);
  visit_named_descendants(binding->scope, false, [&](const TSNode& node) {
    const std::string_view type = ts_node_type(node);
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
    if (left_type != "identifier" || node_text(left, source) != name) {
      return;
    }
    const auto same = local_binding(left, name, source);
    if (!same || !ts_node_eq(same->declarator, binding->declarator)) {
      return;  // a nested declaration of the same name
    }
    if (type == "augmented_assignment_expression" || !ts_node_eq(enclosing_function(node), use_function)) {
      unknowable = true;  // updated in place, or set by a callback that runs who knows when (`beforeAll`)
    } else if (ts_node_start_byte(node) < use_start) {
      values.push_back(unwrap_expression(ts_node_child_by_field_name(node, "right", 5)));
    }
  });
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

// Where a request's method comes from: a fixed verb, or a parameter of the
// enclosing function (`fetch(url, { method })` in `request(method, path)`).
struct MethodSource {
  std::string verb;
  int parameter = -1;
  std::vector<std::string> choices;  // `method: on ? 'POST' : 'DELETE'`: each verb is sent
};

// The `method` of an options object literal: `method: 'POST'` is fixed,
// `method` / `method: m` naming a parameter of the enclosing function is that
// parameter; absent or anything else is empty.
[[nodiscard]] MethodSource options_method(const TSNode& options, const std::vector<std::string>& parameters,
                                          std::string_view source) {
  const TSNode object = unwrap_expression(options);
  if (ts_node_is_null(object) || std::string_view(ts_node_type(object)) != "object") {
    return {};
  }
  const auto count = ts_node_named_child_count(object);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode member = ts_node_named_child(object, index);
    const std::string_view member_type = ts_node_type(member);
    if (member_type == "shorthand_property_identifier" && node_text(member, source) == "method") {
      return MethodSource{.parameter = parameter_index(parameters, "method")};
    }
    if (member_type != "pair" || strip_string_quotes(field_text(member, "key", source)) != "method") {
      continue;
    }
    const TSNode value = unwrap_expression(ts_node_child_by_field_name(member, "value", 5));
    if (ts_node_is_null(value)) {
      return {};
    }
    if (is_string_value(value)) {
      return MethodSource{.verb = upper_verb(strip_string_quotes(node_text(value, source)))};
    }
    if (std::string_view(ts_node_type(value)) == "identifier") {
      return MethodSource{.parameter = parameter_index(parameters, node_text(value, source))};
    }
    if (std::string_view(ts_node_type(value)) == "ternary_expression") {
      auto consequence = literal_verb(ts_node_child_by_field_name(value, "consequence", 11), source);
      auto alternative = literal_verb(ts_node_child_by_field_name(value, "alternative", 11), source);
      if (!consequence.empty() && !alternative.empty()) {
        return MethodSource{.choices = {std::move(consequence), std::move(alternative)}};
      }
    }
    return {};
  }
  return {};
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
    return method.verb == other.method.verb && method.parameter == other.method.parameter && path == other.path &&
           tail == other.tail && base == other.base;
  }
};

constexpr int kMaxWrapperDepth = 3;

std::vector<ClientCall> analyze_client_call(const TSNode& node, const ExtractionContext& context, int depth);

// A function of this file as a wrapper: the one shape every forwarding client
// call in its body agrees on (`patch(url) { return this.api.patch(url) }` is a
// PATCH wrapper under the instance's baseURL). Empty when no call forwards a
// parameter as the path, or two calls disagree.
[[nodiscard]] std::optional<ClientCall> wrapper_shape(const TSNode& function, const ExtractionContext& context, int depth) {
  if (depth > kMaxWrapperDepth) {
    return std::nullopt;
  }
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
  MethodSource method{.verb = shape.method.verb};
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
  if (method.parameter >= 0 || !url.resolvable || url.base_index >= 0) {
    unresolved();
    return;
  }
  const auto joined = join_client_path(prefix, url.path);
  if (method.verb.empty() && shape.tail + 1 < count) {
    method.verb = options_method(ts_node_named_child(arguments, static_cast<std::uint32_t>(shape.tail + 1)),
                                 scope.parameters, context.source).verb;
  }
  if (method.verb.empty()) {
    method.verb = "GET";
  }
  out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = std::move(client), .method = method,
                           .path = joined && !joined->empty() && (joined->front() == '/' || joined->starts_with("${")) ? *joined : std::string{}});
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
      if (auto method = options_method(argument, {}, context.source).verb; !method.empty()) {
        descriptor = "O" + method;
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
    out.push_back(ClientCall{.kind = ClientCall::Kind::Consumer, .client = client,
                             .method = MethodSource{.verb = method.verb},
                             .path = url.resolvable ? url.path : std::string{}});
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
// (`make_id(relative_path + ":" + method_name)`) so they land on a real node.
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
      const auto method_id = make_id(context.relative_path + ":" + node_text(name_node, context.source));
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
  const std::string file_id = make_id(context.relative_path);
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
          add_edge(id, make_id(context.relative_path + ":schema:" + name), "RESPONDS_WITH");
        }
        for (const auto& name : accepts) {
          add_edge(id, make_id(context.relative_path + ":schema:" + name), "ACCEPTS");
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
  const std::string file_id = make_id(context.relative_path);
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
    const auto id = make_id(context.relative_path + ":schema:" + name);
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
                                    .target = make_id(context.relative_path + ":schema:" + target),
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
