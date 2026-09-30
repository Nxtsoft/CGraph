#include "cgraph/python_extractor.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

extern "C" const TSLanguage* tree_sitter_python();

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

// A dotted module spec resolves to a path-like key resolve_imports can match:
// relative dots walk up from the importing file's directory ("." = same
// package, ".." = parent), the dotted remainder becomes slashes. An absolute
// module ("itsdangerous.signer") keeps no anchor — resolve_imports suffix-
// matches it against project files (extension-stripped, __init__-aware).
[[nodiscard]] std::string resolve_python_module_spec(const std::string& source_file, const std::string& spec) {
  std::size_t dots = 0;
  while (dots < spec.size() && spec[dots] == '.') {
    ++dots;
  }
  std::string rest = spec.substr(dots);
  std::ranges::replace(rest, '.', '/');
  if (dots == 0) {
    return rest;
  }
  std::filesystem::path base = std::filesystem::path(source_file).parent_path();
  for (std::size_t up = 1; up < dots; ++up) {
    base = base.parent_path();
  }
  return rest.empty() ? base.generic_string() : (base / rest).lexically_normal().generic_string();
}

// The stub id input for a dotted module spec: an absolute module keeps
// "import-<kind>:"; a relative one is resolved against the project-relative
// directory, walking up with ".." segments (not parent_path, which stops at the
// root), in the relative namespace (relative_import_stub). So a root-level
// `from .config import X` never shares an id with an absolute
// `from config import X`, and an import that climbs above the root stays
// distinct from one that stops at it.
[[nodiscard]] std::string python_module_stub(std::string_view kind, const std::string& relative_path, const std::string& spec) {
  std::size_t dots = 0;
  while (dots < spec.size() && spec[dots] == '.') {
    ++dots;
  }
  std::string rest = spec.substr(dots);
  std::ranges::replace(rest, '.', '/');
  if (dots == 0) {
    return "import-" + std::string(kind) + ":" + rest;
  }
  std::filesystem::path joined = std::filesystem::path(relative_path).parent_path();
  for (std::size_t up = 1; up < dots; ++up) {
    joined /= "..";
  }
  if (!rest.empty()) {
    joined /= rest;
  }
  return relative_import_stub(kind, joined);
}

// Emits the same stub shape the JS handler does — a `module` stub with an
// `import_path` for resolve_imports to collapse onto the real file node, plus
// an `import` stub per imported name so `from x import Thing` can bind to the
// declared symbol. Stub ids are namespaced so they can never squat a real
// node's id (#42). The old handler emitted a dead-end node (whole statement as
// label, no import_path, no edges), which is why Python graphs carried no
// import relations at all (issue #45).
void python_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  const std::string_view statement_type = ts_node_type(node);
  const std::string file_id = make_id(context.relative_path);

  // `spec` is resolved twice: against the absolute source file for import_path
  // (resolve_imports looks it up by the file nodes' absolute source paths) and
  // against the project-relative path for the stub's id, which like every other
  // id must not carry the checkout's location.
  const auto add_module_stub = [&](const std::string& spec, const std::string& label) -> std::string {
    const auto resolved = resolve_python_module_spec(context.source_file, spec);
    const auto module_id = make_id(python_module_stub("module", context.relative_path, spec));
    fragment.nodes.push_back(Node{
        .id = module_id,
        .label = label,
        .source_location = SourceLocation{.start_line = 1, .end_line = 1},
        .kind = "module",
        .confidence = Confidence::Extracted,
        .properties = {{"import_path", resolved}},
    });
    fragment.edges.push_back(Edge{
        .source = file_id,
        .target = module_id,
        .relation = "imports_from",
        .confidence = Confidence::Extracted,
    });
    return module_id;
  };

  if (statement_type == "import_statement") {
    // `import a.b, c as d` — each named child is a dotted_name or aliased_import.
    const auto child_count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < child_count; ++index) {
      auto child = ts_node_named_child(node, index);
      if (std::string_view(ts_node_type(child)) == "aliased_import") {
        child = ts_node_child_by_field_name(child, "name", 4);
        if (ts_node_is_null(child)) {
          continue;
        }
      }
      const auto spec = node_text(child, context.source);
      if (!spec.empty()) {
        add_module_stub(spec, spec);
      }
    }
    return;
  }
  if (statement_type != "import_from_statement") {
    return;
  }

  const auto module_name = ts_node_child_by_field_name(node, "module_name", 11);
  if (ts_node_is_null(module_name)) {
    return;
  }
  const auto spec = node_text(module_name, context.source);
  if (spec.empty()) {
    return;
  }
  add_module_stub(spec, spec);
  const auto resolved = resolve_python_module_spec(context.source_file, spec);
  const auto symbol_stub = python_module_stub("symbol", context.relative_path, spec);

  // `from m import a, b as c` — the imported names are the `name`-field children
  // after module_name (dotted_name or aliased_import). A wildcard import has none.
  const auto child_count = ts_node_named_child_count(node);
  bool past_module = false;
  for (std::uint32_t index = 0; index < child_count; ++index) {
    auto child = ts_node_named_child(node, index);
    if (ts_node_eq(child, module_name)) {
      past_module = true;
      continue;
    }
    if (!past_module) {
      continue;
    }
    std::string alias;
    if (std::string_view(ts_node_type(child)) == "aliased_import") {
      if (const auto alias_node = ts_node_child_by_field_name(child, "alias", 5); !ts_node_is_null(alias_node)) {
        alias = node_text(alias_node, context.source);
      }
      child = ts_node_child_by_field_name(child, "name", 4);
      if (ts_node_is_null(child)) {
        continue;
      }
    }
    if (std::string_view(ts_node_type(child)) != "dotted_name" &&
        std::string_view(ts_node_type(child)) != "identifier") {
      continue;
    }
    const auto name = node_text(child, context.source);
    if (name.empty()) {
      continue;
    }
    const auto symbol_id = make_id(symbol_stub + ":" + name);
    fragment.nodes.push_back(Node{
        .id = symbol_id,
        .label = name,
        .source_location = source_location(node),
        .kind = "import",
        .confidence = Confidence::Extracted,
        .properties = {{"import_path", resolved}},
    });
    Edge edge{
        .source = file_id,
        .target = symbol_id,
        .relation = "imports",
        .confidence = Confidence::Extracted,
    };
    // `from m import router as setup_router`: this file's code says
    // `setup_router`. The alias rides on this file's own edge, not on the stub,
    // because every file importing `m.router` shares the stub; resolve_imports
    // keeps it on the relinked edge and build_relation_scopes binds it.
    if (!alias.empty() && alias != name) {
      edge.properties.emplace("alias", alias);
    }
    fragment.edges.push_back(std::move(edge));
  }
}

void python_member_handler(const TSNode& node, const ExtractionContext& context,
                           const std::string& owner_id, Fragment& fragment) {
  const auto name_node = ts_node_child_by_field_name(node, "name", 4);
  const auto body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(name_node) || ts_node_is_null(body)) return;
  const auto owner_name = node_text(name_node, context.source);
  for (std::uint32_t i = 0; i < ts_node_named_child_count(body); ++i) {
    auto member = ts_node_named_child(body, i);
    while (!ts_node_is_null(member) && std::string_view(ts_node_type(member)) == "assignment") {
      const auto annotation = ts_node_child_by_field_name(member, "type", 4);
      Properties properties;
      if (!ts_node_is_null(annotation)) properties.emplace("type_text", node_text(annotation, context.source));
      const std::function<void(TSNode)> emit_target = [&](const TSNode target) {
        if (ts_node_is_null(target)) return;
        const std::string_view kind = ts_node_type(target);
        if (kind == "identifier") {
          add_field_node(context, owner_id, owner_name, node_text(target, context.source),
                         source_location(member), properties, fragment);
        } else if (kind == "pattern_list" || kind == "tuple_pattern" || kind == "list_pattern" || kind == "list_splat_pattern") {
          for (std::uint32_t j = 0; j < ts_node_named_child_count(target); ++j) emit_target(ts_node_named_child(target, j));
        }
      };
      emit_target(ts_node_child_by_field_name(member, "left", 4));
      member = ts_node_child_by_field_name(member, "right", 5);
    }
  }
}


// HTTP contract facts for resolve_contracts (contracts.hpp), in the shapes
// FastAPI registers routes with:
//   router = APIRouter(prefix="/project")      a chain (`variable` + route_prefix)
//   @router.post("/{project_id}/setup")        a route on it, handled by the def
//   app.include_router(router, prefix="/v1")   a mount, composed across files
// The same three facts the JavaScript extractor emits for Express/Elysia, so
// resolve_contracts composes full paths without knowing the language.

// The text of a plain string literal (`"/x"`, `'/x'`, `r"/x"`, an f-string with
// no `{...}`); nullopt for anything else, including an f-string that
// interpolates: its value is not knowable here.
[[nodiscard]] std::optional<std::string> python_string_literal(const TSNode& node, std::string_view source) {
  if (ts_node_is_null(node) || std::string_view(ts_node_type(node)) != "string") {
    return std::nullopt;
  }
  std::uint32_t begin = ts_node_start_byte(node);
  std::uint32_t end = ts_node_end_byte(node);
  for (std::uint32_t index = 0; index < ts_node_child_count(node); ++index) {
    const auto child = ts_node_child(node, index);
    const std::string_view type = ts_node_type(child);
    if (type == "interpolation") {
      return std::nullopt;
    }
    if (type == "string_start") {
      begin = ts_node_end_byte(child);
    } else if (type == "string_end") {
      end = ts_node_start_byte(child);
    }
  }
  if (begin > end || end > source.size()) {
    return std::nullopt;
  }
  return std::string(source.substr(begin, end - begin));
}

struct PythonCallArguments {
  std::vector<TSNode> positional;
  std::vector<std::pair<std::string, TSNode>> keywords;

  [[nodiscard]] std::optional<TSNode> keyword(std::string_view name) const {
    for (const auto& [key, value] : keywords) {
      if (key == name) {
        return value;
      }
    }
    return std::nullopt;
  }
};

[[nodiscard]] PythonCallArguments python_call_arguments(const TSNode& call, std::string_view source) {
  PythonCallArguments out;
  const auto arguments = ts_node_child_by_field_name(call, "arguments", 9);
  if (ts_node_is_null(arguments) || std::string_view(ts_node_type(arguments)) != "argument_list") {
    return out;
  }
  for (std::uint32_t index = 0; index < ts_node_named_child_count(arguments); ++index) {
    const auto argument = ts_node_named_child(arguments, index);
    const std::string_view type = ts_node_type(argument);
    if (type == "keyword_argument") {
      const auto name = ts_node_child_by_field_name(argument, "name", 4);
      const auto value = ts_node_child_by_field_name(argument, "value", 5);
      if (!ts_node_is_null(name) && !ts_node_is_null(value)) {
        out.keywords.emplace_back(node_text(name, source), value);
      }
    } else if (type != "comment" && type != "list_splat" && type != "dictionary_splat") {
      out.positional.push_back(argument);
    }
  }
  return out;
}

// `X.attr(...)` with X a bare identifier: {X, attr}. Empty for anything else.
[[nodiscard]] std::pair<std::string, std::string> python_member_call(const TSNode& call, std::string_view source) {
  if (ts_node_is_null(call) || std::string_view(ts_node_type(call)) != "call") {
    return {};
  }
  const auto callee = ts_node_child_by_field_name(call, "function", 8);
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "attribute") {
    return {};
  }
  const auto object = ts_node_child_by_field_name(callee, "object", 6);
  const auto attribute = ts_node_child_by_field_name(callee, "attribute", 9);
  if (ts_node_is_null(object) || ts_node_is_null(attribute) || std::string_view(ts_node_type(object)) != "identifier") {
    return {};
  }
  return {node_text(object, source), node_text(attribute, source)};
}

// An assignment directly in the module body, where its name is a module global.
// The grammar may or may not surface the `expression_statement` around it.
[[nodiscard]] bool is_module_assignment(const TSNode& assignment) {
  auto parent = ts_node_parent(assignment);
  if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "expression_statement") {
    parent = ts_node_parent(parent);
  }
  return !ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "module";
}

// True when `node` sits inside a function body: a name there may be a local or
// parameter (`def create_app(): app = FastAPI(); @app.get(...)`), not the
// module's router.
[[nodiscard]] bool inside_function(TSNode node) {
  for (auto parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (std::string_view(ts_node_type(parent)) == "function_definition") {
      return true;
    }
  }
  return false;
}

// The route-prefix keyword a FastAPI router constructor takes, when the call
// constructs one: APIRouter(prefix=); FastAPI() is a root chain that takes none.
// Flask is not modelled: register_blueprint(url_prefix=) REPLACES the
// blueprint's own prefix instead of composing with it, which the mount walk in
// resolve_contracts does not express.
[[nodiscard]] std::optional<std::string_view> router_prefix_keyword(const TSNode& call, std::string_view source) {
  if (ts_node_is_null(call) || std::string_view(ts_node_type(call)) != "call") {
    return std::nullopt;
  }
  auto callee = ts_node_child_by_field_name(call, "function", 8);
  if (!ts_node_is_null(callee) && std::string_view(ts_node_type(callee)) == "attribute") {
    callee = ts_node_child_by_field_name(callee, "attribute", 9);  // `fastapi.APIRouter(...)`
  }
  if (ts_node_is_null(callee) || std::string_view(ts_node_type(callee)) != "identifier") {
    return std::nullopt;
  }
  const auto name = node_text(callee, source);
  if (name == "APIRouter") return "prefix";
  if (name == "FastAPI") return "";
  return std::nullopt;
}

// `router = APIRouter(prefix="/project")` at module level: a `variable` node the
// decorators and mounts name. A prefix that is not a string literal leaves the
// chain out, so routes on it count as unresolved rather than minting endpoints
// at a wrong path.
void python_router_variable(const TSNode& assignment, const ExtractionContext& context, Fragment& fragment) {
  if (!is_module_assignment(assignment)) {
    return;
  }
  const auto left = ts_node_child_by_field_name(assignment, "left", 4);
  const auto right = ts_node_child_by_field_name(assignment, "right", 5);
  if (ts_node_is_null(left) || std::string_view(ts_node_type(left)) != "identifier") {
    return;
  }
  const auto keyword = router_prefix_keyword(right, context.source);
  if (!keyword) {
    return;
  }
  std::string prefix;
  if (!keyword->empty()) {
    if (const auto value = python_call_arguments(right, context.source).keyword(*keyword)) {
      const auto literal = python_string_literal(*value, context.source);
      if (!literal) {
        return;
      }
      prefix = *literal;
    }
  }
  auto name = node_text(left, context.source);
  auto id = make_id(context.relative_path + ":" + name);
  fragment.nodes.push_back(Node{
      .id = id,
      .label = std::move(name),
      .source_file = context.source_file,
      .source_location = source_location(assignment),
      .kind = "variable",
      .confidence = Confidence::Extracted,
  });
  if (!prefix.empty()) {
    fragment.nodes.back().properties.emplace("route_prefix", std::move(prefix));
  }
  // Contained by its file, as a JavaScript router variable is: an import that
  // now binds to the router instead of the file still reaches the file.
  fragment.edges.push_back(Edge{
      .source = make_id(context.relative_path),
      .target = std::move(id),
      .relation = "contains",
      .confidence = Confidence::Extracted,
  });
}

// `app.include_router(router, prefix="/v1")`: the chain named by the first
// argument is served under the mounting chain. A mount inside a function, one whose prefix is not a
// literal, and one whose router is not a bare name (`users.router`) cannot be
// composed; each is still recorded so resolve_contracts counts it unresolved.
void python_router_mount(const TSNode& call, const ExtractionContext& context, std::vector<RawRelation>& out) {
  const auto [parent, method] = python_member_call(call, context.source);
  if (method != "include_router") {
    return;
  }
  const auto arguments = python_call_arguments(call, context.source);
  std::optional<TSNode> child = arguments.positional.empty() ? arguments.keyword("router")
                                                             : std::optional<TSNode>{arguments.positional.front()};
  if (!child) {
    return;
  }
  std::string prefix;
  bool resolvable = !inside_function(call);
  if (const auto value = arguments.keyword("prefix")) {
    const auto literal = python_string_literal(*value, context.source);
    resolvable = resolvable && literal.has_value();
    prefix = literal.value_or(std::string{});
  }
  out.push_back(RawRelation{
      .source_id = resolvable ? make_id(context.relative_path + ":" + parent) : std::string{},
      .target_label = node_text(*child, context.source),
      .relation = "mounts",
      .context = std::move(prefix),
      .source_file = context.source_file,
  });
}

void python_extra_walk(const TSNode& node, const ExtractionContext& context, const std::string& /*function_scope*/,
                       Fragment& fragment, std::vector<RawCall>& /*raw_calls*/, std::vector<RawRelation>& raw_relations) {
  const std::string_view type = ts_node_type(node);
  if (type == "assignment") {
    python_router_variable(node, context, fragment);
  } else if (type == "call") {
    python_router_mount(node, context, raw_relations);
  }
}

// `@router.get("/x")`, `@app.api_route("/x", methods=["GET", "POST"])` on a
// def: one route fact per method,
// handled by the def. The chain is named only for a def outside any function
// (a module global); a path or method list that is not literal records the
// route with nothing resolve_contracts can place, so it counts unresolved.
void python_route_decorators(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                             std::vector<RawRelation>& out) {
  if (std::string_view(ts_node_type(node)) != "function_definition") {
    return;
  }
  const auto decorated = ts_node_parent(node);
  if (ts_node_is_null(decorated) || std::string_view(ts_node_type(decorated)) != "decorated_definition") {
    return;
  }
  const bool module_scope = !inside_function(decorated);
  for (std::uint32_t index = 0; index < ts_node_named_child_count(decorated); ++index) {
    const auto decorator = ts_node_named_child(decorated, index);
    if (std::string_view(ts_node_type(decorator)) != "decorator" || ts_node_named_child_count(decorator) == 0) {
      continue;
    }
    const auto call = ts_node_named_child(decorator, 0);
    const auto [chain, method] = python_member_call(call, context.source);
    std::vector<std::string> verbs;
    const bool multi = method == "api_route";
    if (multi) {
      // api_route without methods= serves GET.
      verbs.emplace_back("get");
    } else if (method == "get" || method == "post" || method == "put" || method == "patch" || method == "delete" ||
               method == "head" || method == "options") {
      verbs.push_back(method);
    } else {
      continue;
    }
    const auto arguments = python_call_arguments(call, context.source);
    if (multi) {
      if (const auto methods = arguments.keyword("methods")) {
        verbs.clear();
        const std::string_view list_type = ts_node_type(*methods);
        bool literal = list_type == "list" || list_type == "tuple" || list_type == "set";
        for (std::uint32_t item = 0; literal && item < ts_node_named_child_count(*methods); ++item) {
          const auto verb = python_string_literal(ts_node_named_child(*methods, item), context.source);
          literal = verb.has_value();
          if (verb) {
            std::string lower = *verb;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            verbs.push_back(std::move(lower));
          }
        }
        if (!literal) {
          verbs.assign(1, std::string{"?"});  // not a verb: counted unresolved
        }
      }
    }
    std::optional<TSNode> path_node = arguments.positional.empty()
                                          ? arguments.keyword("path")
                                          : std::optional<TSNode>{arguments.positional.front()};
    const auto path = path_node ? python_string_literal(*path_node, context.source) : std::nullopt;
    for (const auto& verb : verbs) {
      out.push_back(RawRelation{
          .source_id = node_id,
          .target_label = module_scope && path ? chain : std::string{},
          .relation = "route",
          .context = verb + " " + path.value_or(std::string{}),
          .source_file = context.source_file,
      });
    }
  }
}

}  // namespace

LanguageConfig python_language_config() {
  return LanguageConfig{
      .name = "python",
      .grammar_name = "tree-sitter-python",
      .extensions = {".py", ".pyw"},
      .class_node_types = {"class_definition"},
      .function_node_types = {"function_definition"},
      .import_node_types = {"import_statement", "import_from_statement"},
      .call_node_types = {"call"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
      // `obj.method(...)` / `self.helper(...)`: record the bare attribute name
      // as a member call so it can resolve same-file, or project-wide when the
      // name uniquely names a method.
      .call_member_node_types = {"attribute"},
      .call_member_field = "attribute",
      .import_handler = python_import_handler,
      .extra_walk = python_extra_walk,
      .relation_handler = python_route_decorators,
      .extract_members = true,
      .member_handler = python_member_handler,
  };
}

ExtractionResult extract_python(const ExtractionContext& context) {
  auto config = python_language_config();
  intern_node_symbols(config, tree_sitter_python());
  return extract_with_config(tree_sitter_python(), config, context);
}

}  // namespace cgraph
