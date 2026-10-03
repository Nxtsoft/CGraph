#include "cgraph/configured_extractors.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/cpp_extractor.hpp"
#include "cgraph/env_contracts.hpp"
#include "cgraph/header_contracts.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/non_grammar_extractors.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/python_extractor.hpp"
#include "cgraph/spring_actuator.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

extern "C" const TSLanguage* tree_sitter_c();
extern "C" const TSLanguage* tree_sitter_cpp();
extern "C" const TSLanguage* tree_sitter_c_sharp();
extern "C" const TSLanguage* tree_sitter_go();
extern "C" const TSLanguage* tree_sitter_groovy();
extern "C" const TSLanguage* tree_sitter_java();
extern "C" const TSLanguage* tree_sitter_javascript();
extern "C" const TSLanguage* tree_sitter_kotlin();
extern "C" const TSLanguage* tree_sitter_python();
extern "C" const TSLanguage* tree_sitter_ruby();
extern "C" const TSLanguage* tree_sitter_rust();
extern "C" const TSLanguage* tree_sitter_scala();
extern "C" const TSLanguage* tree_sitter_typescript();
extern "C" const TSLanguage* tree_sitter_tsx();

// Defined below (next to go_import_handler); forward-declared so the Kotlin and
// Java resolvers above it can reuse the same byte-safe node-text helper.
[[nodiscard]] std::string go_node_text(const TSNode& node, std::string_view source);

[[nodiscard]] TSNode member_field(const TSNode& node, std::string_view name) {
  return ts_node_child_by_field_name(node, name.data(), static_cast<std::uint32_t>(name.size()));
}

void emit_member(const TSNode& member, const ExtractionContext& context,
                 const std::string& owner_id, const std::string& owner_name,
                 std::string name, std::string type_text, Fragment& fragment,
                 Properties properties = {}) {
  if (!type_text.empty()) properties.emplace("type_text", std::move(type_text));
  const auto start = ts_node_start_point(member);
  const auto end = ts_node_end_point(member);
  add_field_node(context, owner_id, owner_name, std::move(name),
                 SourceLocation{.start_line = start.row + 1, .start_column = start.column,
                                .end_line = end.row + 1, .end_column = end.column},
                 std::move(properties), fragment);
}

void go_member_handler(const TSNode& node, const ExtractionContext& context,
                       const std::string& owner_id, Fragment& fragment) {
  const auto type = member_field(node, "type");
  if (ts_node_is_null(type) || std::string_view(ts_node_type(type)) != "struct_type") return;
  const auto owner_name = go_node_text(member_field(node, "name"), context.source);
  const auto body = ts_node_named_child(type, 0);
  for (std::uint32_t i = 0; i < ts_node_named_child_count(body); ++i) {
    const auto member = ts_node_named_child(body, i);
    if (std::string_view(ts_node_type(member)) != "field_declaration") continue;
    const auto type_node = member_field(member, "type");
    auto type_text = go_node_text(type_node, context.source);
    bool named = false;
    for (std::uint32_t j = 0; j < ts_node_child_count(member); ++j) {
      const auto field = ts_node_field_name_for_child(member, j);
      if (field == nullptr || std::string_view(field) != "name") continue;
      named = true;
      emit_member(member, context, owner_id, owner_name,
                  go_node_text(ts_node_child(member, j), context.source), type_text, fragment);
    }
    if (!named) {
      auto name_node = type_node;
      const std::string_view kind = ts_node_type(name_node);
      if (kind == "generic_type") name_node = member_field(name_node, "type");
      if (std::string_view(ts_node_type(name_node)) == "qualified_type") name_node = member_field(name_node, "name");
      const auto label = go_node_text(name_node, context.source);
      if (std::string_view(ts_node_type(ts_node_child(member, 0))) == "*") type_text = "*" + type_text;
      emit_member(member, context, owner_id, owner_name, label, type_text, fragment);
    }
  }
}

void rust_member_handler(const TSNode& node, const ExtractionContext& context,
                         const std::string& owner_id, Fragment& fragment) {
  const auto body = member_field(node, "body");
  if (ts_node_is_null(body)) return;
  const auto owner_name = go_node_text(member_field(node, "name"), context.source);
  const bool tuple = std::string_view(ts_node_type(body)) == "ordered_field_declaration_list";
  std::uint32_t position = 0;
  for (std::uint32_t i = 0; i < ts_node_child_count(body); ++i) {
    const auto member = ts_node_child(body, i);
    const std::string_view kind = ts_node_type(member);
    if (tuple) {
      const auto field = ts_node_field_name_for_child(body, i);
      if (field != nullptr && std::string_view(field) == "type") {
        emit_member(member, context, owner_id, owner_name, std::to_string(position++),
                    go_node_text(member, context.source), fragment);
      }
      continue;
    }
    // `function_item`, `function_signature_item` and `type_item` are already
    // graph nodes of their own (rust_config's function_node_types and
    // type_node_types), and a member node would reuse their id and overwrite
    // them -- erasing the `interface_method` tag trait dispatch reads.
    if (kind != "field_declaration" && kind != "enum_variant" && kind != "associated_type" &&
        kind != "const_item") continue;
    auto type_text = go_node_text(member_field(member, "type"), context.source);
    if (kind == "enum_variant") type_text = go_node_text(member_field(member, "body"), context.source);
    emit_member(member, context, owner_id, owner_name,
                go_node_text(member_field(member, "name"), context.source), type_text, fragment);
  }
}

void java_member_handler(const TSNode& node, const ExtractionContext& context,
                         const std::string& owner_id, Fragment& fragment) {
  const auto owner_name = go_node_text(member_field(node, "name"), context.source);
  const auto emit = [&](const TSNode& member, const TSNode& declarator, bool readonly) {
    auto type_text = go_node_text(member_field(member, "type"), context.source);
    type_text += go_node_text(member_field(declarator, "dimensions"), context.source);
    emit_member(declarator, context, owner_id, owner_name,
                go_node_text(member_field(declarator, "name"), context.source), type_text,
                fragment, {{"readonly", readonly ? "true" : "false"}});
  };
  const auto parameters = member_field(node, "parameters");
  if (!ts_node_is_null(parameters)) {
    for (std::uint32_t i = 0; i < ts_node_named_child_count(parameters); ++i) {
      const auto parameter = ts_node_named_child(parameters, i);
      if (std::string_view(ts_node_type(parameter)) == "formal_parameter") emit(parameter, parameter, true);
    }
  }
  const auto body = member_field(node, "body");
  if (ts_node_is_null(body)) return;
  const auto emit_fields = [&](const TSNode& container) {
    for (std::uint32_t i = 0; i < ts_node_named_child_count(container); ++i) {
      const auto member = ts_node_named_child(container, i);
      const std::string_view kind = ts_node_type(member);
      if (kind == "enum_constant") {
        emit_member(member, context, owner_id, owner_name,
                    go_node_text(member_field(member, "name"), context.source), {}, fragment);
        continue;
      }
      if (kind != "field_declaration" && kind != "constant_declaration") continue;
      bool readonly = kind == "constant_declaration";
      for (std::uint32_t j = 0; j < ts_node_named_child_count(member); ++j) {
        const auto child = ts_node_named_child(member, j);
        if (std::string_view(ts_node_type(child)) != "modifiers") continue;
        for (std::uint32_t k = 0; k < ts_node_child_count(child); ++k) {
          readonly = readonly || std::string_view(ts_node_type(ts_node_child(child, k))) == "final";
        }
      }
      for (std::uint32_t j = 0; j < ts_node_named_child_count(member); ++j) {
        const auto declarator = ts_node_named_child(member, j);
        if (std::string_view(ts_node_type(declarator)) == "variable_declarator") emit(member, declarator, readonly);
      }
    }
  };
  emit_fields(body);
  for (std::uint32_t i = 0; i < ts_node_named_child_count(body); ++i) {
    const auto child = ts_node_named_child(body, i);
    if (std::string_view(ts_node_type(child)) == "enum_body_declarations") emit_fields(child);
  }
}

[[nodiscard]] LanguageConfig c_config() {
  LanguageConfig config{
      .name = "c",
      .grammar_name = "tree-sitter-c",
      .extensions = {".c", ".h"},
      .class_node_types = {"struct_specifier", "union_specifier", "enum_specifier"},
      .function_node_types = {"function_definition"},
      .import_node_types = {"preproc_include"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name", "declarator"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
      // `obj.method()`, `ptr->method()`, and `obj.*pm()` are all `field_expression`
      // in the C and C++ grammars, with the bare name in the `field` field -- so
      // one entry covers every member-call spelling. Go and C# already declared
      // their equivalents (`selector_expression`/`field`,
      // `member_access_expression`/`name`); C and C++ declared neither, so
      // add_raw_call fell through to the verbatim receiver expression and recorded
      // `state.stats.record` as the callee name, which matched nothing. A member
      // call stays scoped to the caller's own file, because the receiver type is
      // unknown and a project-wide name match would be a guess.
      .call_member_node_types = {"field_expression"},
      .call_member_field = "field",
      .resolve_callee_name = cpp_callee_name,
      .resolve_callee_scope = cpp_callee_scope,
      // Grammar-driven callee naming. A text rule cannot do this job: `::` shows up
      // in nine distinct callee node types, and `ns::make<zoo::Beast>` reduced at
      // its last `::` yields `Beast>` -- a fabricated call to an unrelated struct.
  };
  // `#include` -> imports, struct members -> defines, member/param/return types
  // -> references. cpp_relation_handler also emits inherits, which is a no-op for
  // C (no base classes). Shared by the C and C++ configs.
  config.import_handler = cpp_import_handler;
  config.relation_handler = cpp_relation_handler;
  config.extra_walk = cpp_field_walk;
  config.class_requires_body = true;
  // A `function_definition` has no `name` field, so without this the label would
  // be the declarator's raw text -- the whole declaration, signature included --
  // and a bare callee name at a call site could never match it. See
  // cpp_extractor.hpp.
  config.resolve_function_name = cpp_function_name;
  return config;
}

[[nodiscard]] LanguageConfig cpp_config() {
  auto config = c_config();
  config.name = "cpp";
  config.grammar_name = "tree-sitter-cpp";
  config.extensions = {".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx"};
  config.class_node_types.push_back("class_specifier");
  // `namespace_definition` is deliberately NOT a class node. Node ids are
  // per-file, so `namespace cgraph { }` in N files minted N separate "class"
  // nodes all labelled `cgraph` -- it never grouped anything across files, which
  // is the only thing a namespace node could have been for. Worse, a class
  // parent makes add_containment_edge label every member a `method`, so on this
  // repo 96 of 214 class nodes were one namespace, 416 of 449 `method` edges
  // originated at one, it was the highest-degree node in the entire graph
  // (degree 45, centrality 1.0, god_node), and 92% of connected function pairs
  // routed their shortest path through it -- making `path` answer "both are in
  // namespace cgraph" instead of naming the real call chain.
  //
  // With no node emitted, label_for_node's documented skip path applies: the
  // enclosing scope stays the file node and members attach to it with
  // `contains`. No symbol is lost.
  return config;
}

// A `new Foo()` / `new pkg.Foo<T>()` callee is an object_creation_expression
// whose `type` field is a `_simple_type`, not a plain `name` identifier. Reduce
// it (and a method_invocation's `name` identifier, passed through unchanged) to
// the bare simple type/method name so the call resolves against the class node:
//   type_identifier / identifier  -> its text
//   generic_type (`Foo<T>`)       -> its base type_identifier/scoped_type_identifier
//   scoped_type_identifier (`a.b.Foo`) / scoped_identifier -> the last simple name
// Without this, `new ArrayList<>()` would be labelled `ArrayList<>` (matching
// nothing) and `new com.foo.Bar()` `com.foo.Bar`; a plain `new Circle()` would
// happen to work by text but the generic/qualified forms would silently drop.
[[nodiscard]] std::string java_callee_name(const TSNode& node, const ExtractionContext& context) {
  TSNode cur = node;
  for (int guard = 0; guard < 8 && !ts_node_is_null(cur); ++guard) {
    const std::string_view type = ts_node_type(cur);
    if (type == "identifier" || type == "type_identifier") {
      return go_node_text(cur, context.source);
    }
    if (type == "generic_type") {
      TSNode base = {};
      const auto count = ts_node_named_child_count(cur);
      for (uint32_t index = 0; index < count; ++index) {
        const TSNode child = ts_node_named_child(cur, index);
        const std::string_view child_type = ts_node_type(child);
        if (child_type == "type_identifier" || child_type == "scoped_type_identifier") {
          base = child;
          break;
        }
      }
      if (ts_node_is_null(base)) {
        return {};
      }
      cur = base;
      continue;
    }
    if (type == "scoped_type_identifier" || type == "scoped_identifier") {
      // The simple name is the last type_identifier/identifier child (`a.b.Foo` -> `Foo`).
      TSNode leaf = {};
      const auto count = ts_node_named_child_count(cur);
      for (uint32_t index = 0; index < count; ++index) {
        const TSNode child = ts_node_named_child(cur, index);
        const std::string_view child_type = ts_node_type(child);
        if (child_type == "type_identifier" || child_type == "identifier") {
          leaf = child;
        }
      }
      if (ts_node_is_null(leaf)) {
        return {};
      }
      return go_node_text(leaf, context.source);
    }
    return {};
  }
  return {};
}

// --- Spring MVC request mappings (Kotlin and Java) ---------------------------
// A method annotated @GetMapping/@PostMapping/@PutMapping/@DeleteMapping/
// @PatchMapping, or @RequestMapping with a `method`, is an HTTP handler whose
// path is the enclosing class's @RequestMapping prefix joined with its own. The
// path is absolute (no router chain to compose), so it is emitted as a
// `file_route` fact, which resolve_contracts turns into an endpoint handled by
// the method. The annotation's source text is parsed rather than its tree, so
// one parser serves both grammars. A path that is not a string literal (a
// constant, a template), or a method-level @RequestMapping without a method,
// emits nothing: an endpoint with a wrong path is worse than none.

struct SpringAnnotation {
  std::string name;                 // simple name: "GetMapping", "RequestMapping", ...
  std::vector<std::string> args;    // top-level arguments, verbatim
};

// Splits `text` at top-level commas, ignoring commas inside quotes and brackets.
[[nodiscard]] std::vector<std::string> split_top_level(std::string_view text) {
  std::vector<std::string> parts;
  std::string current;
  int depth = 0;
  bool quoted = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (quoted) {
      current.push_back(ch);
      if (ch == '\\' && i + 1 < text.size()) {
        current.push_back(text[++i]);
      } else if (ch == '"') {
        quoted = false;
      }
      continue;
    }
    if (ch == '"') {
      quoted = true;
    } else if (ch == '(' || ch == '[' || ch == '{') {
      ++depth;
    } else if (ch == ')' || ch == ']' || ch == '}') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      parts.push_back(std::move(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(std::move(current));
  for (auto& part : parts) {
    const auto first = part.find_first_not_of(" \t\r\n");
    const auto last = part.find_last_not_of(" \t\r\n");
    part = first == std::string::npos ? std::string{} : part.substr(first, last - first + 1);
  }
  std::erase_if(parts, [](const std::string& part) { return part.empty(); });
  return parts;
}

[[nodiscard]] std::optional<SpringAnnotation> parse_spring_annotation(std::string_view text) {
  if (text.empty() || text.front() != '@') {
    return std::nullopt;
  }
  std::size_t end = 1;
  while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_' || text[end] == '.')) {
    ++end;
  }
  auto qualified = text.substr(1, end - 1);
  SpringAnnotation annotation{.name = std::string(qualified.substr(qualified.rfind('.') == std::string_view::npos ? 0 : qualified.rfind('.') + 1)), .args = {}};
  const auto open = text.find('(', end);
  const auto close = text.rfind(')');
  if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
    annotation.args = split_top_level(text.substr(open + 1, close - open - 1));
  }
  return annotation;
}

// The string literals of a Spring path value: "x", ["x", "y"], {"x", "y"} or
// arrayOf("x"). nullopt when any element is not a plain literal.
[[nodiscard]] std::optional<std::vector<std::string>> spring_path_literals(std::string_view value) {
  std::string_view inner = value;
  if (inner.starts_with("arrayOf(") && inner.ends_with(")")) {
    inner = inner.substr(8, inner.size() - 9);
  } else if ((inner.starts_with("[") && inner.ends_with("]")) || (inner.starts_with("{") && inner.ends_with("}"))) {
    inner = inner.substr(1, inner.size() - 2);
  }
  std::vector<std::string> paths;
  for (const auto& element : split_top_level(inner)) {
    // Exactly one plain literal: an inner quote means concatenation ("/a" + "/b")
    // or a Kotlin raw string, and `$` a template; none has a readable path.
    if (element.size() < 2 || element.front() != '"' || element.back() != '"' ||
        element.find('$') != std::string::npos ||
        element.substr(1, element.size() - 2).find('"') != std::string::npos) {
      return std::nullopt;
    }
    paths.push_back(element.substr(1, element.size() - 2));
  }
  if (paths.empty()) {
    paths.emplace_back();  // `[]` / `{}`: the prefix alone
  }
  return paths;
}

struct SpringMapping {
  std::vector<std::string> verbs;   // lowercase; empty for a method-less @RequestMapping
  std::vector<std::string> paths;   // "" means the prefix alone
};

// The mapping an annotation declares, or nullopt when it is not a mapping or its
// path cannot be read literally.
[[nodiscard]] std::optional<SpringMapping> spring_mapping(const SpringAnnotation& annotation) {
  static constexpr std::pair<std::string_view, std::string_view> kVerbs[] = {
      {"GetMapping", "get"}, {"PostMapping", "post"}, {"PutMapping", "put"},
      {"DeleteMapping", "delete"}, {"PatchMapping", "patch"}, {"RequestMapping", ""}};
  const auto verb = std::ranges::find(kVerbs, annotation.name, &std::pair<std::string_view, std::string_view>::first);
  if (verb == std::end(kVerbs)) {
    return std::nullopt;
  }
  SpringMapping mapping;
  if (!verb->second.empty()) {
    mapping.verbs.emplace_back(verb->second);
  }
  std::optional<std::vector<std::string>> paths = std::vector<std::string>{""};
  std::vector<std::string> positional;  // Kotlin passes several paths as separate arguments
  bool positional_readable = true;
  for (const auto& arg : annotation.args) {
    const auto equals = arg.find('=');
    const bool named = equals != std::string::npos && arg.find('"') > equals;
    if (!named) {
      if (const auto literals = spring_path_literals(arg)) {
        positional.insert(positional.end(), literals->begin(), literals->end());
      } else {
        positional_readable = false;
      }
      continue;
    }
    auto key = arg.substr(0, equals);
    key.erase(key.find_last_not_of(" \t") + 1);
    auto value = std::string_view(arg).substr(equals + 1);
    value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
    if (key == "value" || key == "path") {
      paths = spring_path_literals(value);
    } else if (key == "method") {
      std::string_view list = value;
      if ((list.starts_with("[") && list.ends_with("]")) || (list.starts_with("{") && list.ends_with("}"))) {
        list = list.substr(1, list.size() - 2);
      }
      for (const auto& item : split_top_level(list)) {
        auto name = item.substr(item.rfind('.') == std::string::npos ? 0 : item.rfind('.') + 1);
        std::ranges::transform(name, name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (is_http_verb(name)) {
          mapping.verbs.push_back(std::move(name));
        }
      }
    }
  }
  if (!positional_readable) {
    return std::nullopt;
  }
  if (!positional.empty()) {
    paths = std::move(positional);
  }
  if (!paths) {
    return std::nullopt;
  }
  mapping.paths = std::move(*paths);
  return mapping;
}

// Annotations on a declaration: the annotation children of its `modifiers`.
[[nodiscard]] std::vector<SpringAnnotation> declaration_annotations(const TSNode& declaration, std::string_view source) {
  std::vector<SpringAnnotation> annotations;
  for (std::uint32_t i = 0; i < ts_node_named_child_count(declaration); ++i) {
    const auto child = ts_node_named_child(declaration, i);
    if (std::string_view(ts_node_type(child)) != "modifiers") {
      continue;
    }
    for (std::uint32_t j = 0; j < ts_node_named_child_count(child); ++j) {
      const auto modifier = ts_node_named_child(child, j);
      if (std::string_view(ts_node_type(modifier)).find("annotation") == std::string_view::npos) {
        continue;
      }
      if (auto parsed = parse_spring_annotation(go_node_text(modifier, source))) {
        annotations.push_back(std::move(*parsed));
      }
    }
  }
  return annotations;
}

[[nodiscard]] bool is_spring_mapping_name(std::string_view name) {
  return name == "GetMapping" || name == "PostMapping" || name == "PutMapping" || name == "DeleteMapping" ||
         name == "PatchMapping" || name == "RequestMapping";
}

// Whether a Kotlin `class_declaration` is an interface: this grammar has no
// interface node, only an `interface` keyword token in place of `class`.
[[nodiscard]] bool has_interface_keyword(const TSNode& declaration) {
  for (std::uint32_t i = 0; i < ts_node_child_count(declaration); ++i) {
    if (std::string_view(ts_node_type(ts_node_child(declaration, i))) == "interface") {
      return true;
    }
  }
  return false;
}

// Whether a class declaration is abstract, sealed or an enum: Spring never
// instantiates it as a controller bean, so a mapping on it is inherited by
// subclasses under THEIR prefix, and minting it here would give a wrong path.
// Kotlin spells these as `inheritance_modifier`/`class_modifier` nodes inside
// `modifiers`, Java as keyword tokens there; reading each modifier's text
// serves both.
[[nodiscard]] bool is_non_instantiable_class(const TSNode& declaration, std::string_view source) {
  for (std::uint32_t i = 0; i < ts_node_named_child_count(declaration); ++i) {
    const auto child = ts_node_named_child(declaration, i);
    if (std::string_view(ts_node_type(child)) != "modifiers") {
      continue;
    }
    for (std::uint32_t j = 0; j < ts_node_child_count(child); ++j) {
      const auto modifier = go_node_text(ts_node_child(child, j), source);
      if (modifier == "abstract" || modifier == "sealed" || modifier == "enum") {
        return true;
      }
    }
  }
  return false;
}

// Which enclosing declarations count, per language. Spring routes only methods
// of a concrete controller class: a method whose nearest enclosing type is an
// interface (an openapi-generator API, a Feign client that CALLS the route), an
// object or companion object, or no type at all (a top-level function) is not a
// handler, and minting it would record a caller as the server.
struct SpringScopes {
  std::span<const std::string_view> methods;     // handler candidates
  std::span<const std::string_view> classes;     // concrete class declarations
  std::span<const std::string_view> non_routed;  // types whose methods are never handlers
};

void spring_route_relations(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                            std::vector<RawRelation>& out, const SpringScopes& scopes) {
  if (std::ranges::find(scopes.methods, std::string_view(ts_node_type(node))) == scopes.methods.end()) {
    return;
  }
  // The first Spring mapping on the method decides; other *Mapping annotations
  // (@MessageMapping, @SubscribeMapping) are not HTTP routes and are skipped.
  std::optional<SpringMapping> method_mapping;
  for (const auto& annotation : declaration_annotations(node, context.source)) {
    if (is_spring_mapping_name(annotation.name)) {
      method_mapping = spring_mapping(annotation);
      if (!method_mapping) {
        return;  // a mapping whose path is not a literal: no endpoint rather than a wrong one
      }
      break;
    }
  }
  if (!method_mapping || method_mapping->verbs.empty()) {
    return;
  }
  // The nearest enclosing type must be a concrete class; its @RequestMapping
  // supplies the prefix(es).
  std::vector<std::string> prefixes{""};
  bool in_class = false;
  for (auto parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    const std::string_view type = ts_node_type(parent);
    if (std::ranges::find(scopes.non_routed, type) != scopes.non_routed.end()) {
      return;
    }
    if (std::ranges::find(scopes.classes, type) == scopes.classes.end()) {
      continue;
    }
    if (has_interface_keyword(parent) || is_non_instantiable_class(parent, context.source)) {
      return;
    }
    for (const auto& annotation : declaration_annotations(parent, context.source)) {
      if (annotation.name == "FeignClient") {
        return;
      }
      if (annotation.name == "RequestMapping") {
        const auto class_mapping = spring_mapping(annotation);
        if (!class_mapping) {
          return;  // an unreadable prefix: every path under it is unknowable
        }
        prefixes = class_mapping->paths;
      }
    }
    in_class = true;
    break;
  }
  if (!in_class) {
    return;
  }
  for (const auto& prefix : prefixes) {
    for (const auto& path : method_mapping->paths) {
      const auto full = join_route_path(prefix.empty() ? "/" : prefix, path);
      for (const auto& verb : method_mapping->verbs) {
        out.push_back(RawRelation{
            .source_id = node_id,
            .target_label = {},
            .relation = "file_route",  // the annotation's path is absolute: no chain to compose
            .context = verb + " " + full,
            .source_file = context.source_file,
        });
      }
    }
  }
}

void kotlin_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                             std::vector<RawRelation>& out) {
  static constexpr std::string_view kMethods[] = {"function_declaration"};
  static constexpr std::string_view kClasses[] = {"class_declaration"};
  static constexpr std::string_view kNonRouted[] = {"object_declaration", "companion_object", "object_literal"};
  spring_route_relations(node, context, node_id, out, {kMethods, kClasses, kNonRouted});
}

void java_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                           std::vector<RawRelation>& out) {
  static constexpr std::string_view kMethods[] = {"method_declaration"};
  static constexpr std::string_view kClasses[] = {"class_declaration", "record_declaration"};
  static constexpr std::string_view kNonRouted[] = {"interface_declaration", "enum_declaration",
                                                    "annotation_type_declaration", "object_creation_expression"};
  spring_route_relations(node, context, node_id, out, {kMethods, kClasses, kNonRouted});
}

[[nodiscard]] LanguageConfig java_config() {
  LanguageConfig config{
      .name = "java",
      .grammar_name = "tree-sitter-java",
      .extensions = {".java"},
      .class_node_types = {"class_declaration", "interface_declaration", "enum_declaration", "record_declaration"},
      .function_node_types = {"method_declaration", "constructor_declaration"},
      .import_node_types = {"import_declaration"},
      .call_node_types = {"method_invocation", "object_creation_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      // method_invocation exposes the callee as `name`; object_creation_expression
      // (a `new Foo()` constructor call) exposes it as `type`. java_callee_name
      // reduces either to the bare simple name.
      .call_accessor_fields = {"name", "type"},
      // `obj.method()` is a method_invocation with an `object` field and no
      // member-access wrapper node, so call_member_node_types cannot see it.
      // The receiver field is what marks it a member call, which is what the
      // interface-dispatch rescue requires.
      .call_receiver_field = "object",
      .interface_node_types = {"interface_declaration"},
  };
  config.extract_members = true;
  config.member_handler = java_member_handler;
  config.resolve_callee_name = java_callee_name;
  config.relation_handler = java_relation_handler;
  config.extra_walk = [](const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                         Fragment& /*fragment*/, std::vector<RawCall>& /*raw_calls*/,
                         std::vector<RawRelation>& raw_relations) {
    java_env_reads(node, context, function_scope_id, raw_relations);
  };
  return config;
}

[[nodiscard]] LanguageConfig csharp_config() {
  return LanguageConfig{
      .name = "csharp",
      .grammar_name = "tree-sitter-c-sharp",
      .extensions = {".cs"},
      .class_node_types = {"class_declaration", "interface_declaration", "struct_declaration",
                           "enum_declaration", "record_declaration", "namespace_declaration"},
      .function_node_types = {"method_declaration", "constructor_declaration",
                              "local_function_statement"},
      .import_node_types = {"using_directive"},
      .call_node_types = {"invocation_expression", "object_creation_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
      // `obj.Method()` / `Type.Static()` targets are member_access_expressions;
      // record the bare member name as a same-file member call, mirroring Go's
      // selector_expression handling (the receiver/type is not name-guessed).
      .call_member_node_types = {"member_access_expression"},
      .call_member_field = "name",
  };
}

[[nodiscard]] LanguageConfig ruby_config() {
  return LanguageConfig{
      .name = "ruby",
      .grammar_name = "tree-sitter-ruby",
      .extensions = {".rb"},
      .class_node_types = {"class", "module"},
      .function_node_types = {"method", "singleton_method"},
      .import_node_types = {"call"},
      .call_node_types = {"call", "command"},
      .name_fields = {"name", "method"},
      .body_fields = {"body"},
      .call_accessor_fields = {"method", "name"},
  };
}

// fwcd/tree-sitter-kotlin exposes NO named fields on its declarations:
// class_declaration, object_declaration, and function_declaration all have empty
// field tables (only function_declaration carries a `receiver` field, which is
// the extension-function receiver type, not the name). So the field-based name
// path in label_for_node finds nothing and every Kotlin symbol is skipped -- the
// reason a Kotlin repo extracts zero nodes today. Resolve the name positionally
// instead: a class or object is named by its `type_identifier` child, a function
// by its `simple_identifier` child (its return type is a `user_type`, and its
// parameters live inside `function_value_parameters`, so the sole top-level
// simple_identifier is the name). Returning empty falls through to
// label_for_node's documented skip path -- an anonymous `object { }` literal has
// no such child and is correctly not a symbol.
[[nodiscard]] std::string kotlin_symbol_name(const TSNode& node, const ExtractionContext& context) {
  const std::string_view node_type = ts_node_type(node);
  const std::string_view wanted =
      (node_type == "function_declaration") ? "simple_identifier" : "type_identifier";
  const auto count = ts_node_named_child_count(node);
  for (uint32_t index = 0; index < count; ++index) {
    const TSNode child = ts_node_named_child(node, index);
    if (std::string_view(ts_node_type(child)) == wanted) {
      return go_node_text(child, context.source);
    }
  }
  return {};
}

// Kotlin's `call_expression` is likewise field-less (no `function` field), so the
// accessor-field path in add_raw_call never reaches a resolver and the callee
// would be labelled with the entire call's text. Given the call node, descend to
// the callee's bare leaf name:
//   call_expression       -> its first named child is the callee expression
//   navigation_expression (`recv.member`) -> the (last) navigation_suffix's
//       simple_identifier -- the bare member name, matched project-wide by name
//       exactly like Java's method_invocation `name`. The receiver type is
//       unknown, so this is deliberately a project-wide name match, not a
//       same-file member call; it is what connects `c.add()` in a test to
//       `Calc.add` in another file.
//   simple_identifier (`f()`, `Widget()`) -> its text (a bare call or a
//       constructor-style invocation, which resolves to the class of that name).
// Anything else -- a call on a literal, an indexing/lambda result -- yields no
// name and the call is dropped rather than guessed.
[[nodiscard]] std::string kotlin_callee_name(const TSNode& node, const ExtractionContext& context) {
  TSNode cur = node;
  if (std::string_view(ts_node_type(cur)) == "call_expression") {
    cur = ts_node_named_child(cur, 0);
  }
  while (!ts_node_is_null(cur)) {
    const std::string_view type = ts_node_type(cur);
    if (type == "simple_identifier") {
      return go_node_text(cur, context.source);
    }
    if (type == "navigation_expression") {
      TSNode suffix = {};
      const auto n = ts_node_named_child_count(cur);
      for (uint32_t i = 0; i < n; ++i) {
        const TSNode child = ts_node_named_child(cur, i);
        if (std::string_view(ts_node_type(child)) == "navigation_suffix") {
          suffix = child;  // keep the last one: `a.b.c` chains suffixes; c wins
        }
      }
      if (ts_node_is_null(suffix)) {
        return {};
      }
      const auto m = ts_node_named_child_count(suffix);
      for (uint32_t i = 0; i < m; ++i) {
        const TSNode child = ts_node_named_child(suffix, i);
        if (std::string_view(ts_node_type(child)) == "simple_identifier") {
          return go_node_text(child, context.source);
        }
      }
      return {};
    }
    // A chained/parenthesized receiver (`foo().bar()`, `(x).y()`): descend to the
    // receiver expression and keep reducing toward the outermost callee name.
    if (type == "call_expression" || type == "parenthesized_expression") {
      cur = ts_node_named_child(cur, 0);
      continue;
    }
    return {};
  }
  return {};
}

// --- HTTP clients (Kotlin Ktor, Go net/http) ---------------------------------
// The same `http_call` / `http_wrapper` facts the JavaScript extractor records
// (contracts.hpp), so resolve_contracts turns them into CONSUMES edges
// unchanged. A client call is:
//   Kotlin  `client.post("$baseUrl/api/v1/x/$id") { ... }`: a get/post/put/
//           patch/delete/head/options member call on a receiver named like an
//           HTTP client (`client`, `httpClient`, `api`); the verb is the member.
//   Go      `http.Get(url)`, `http.NewRequest(method, url, body)`, and any call
//           passing a context, then a method (`http.MethodPost` or a literal
//           "POST"), then a URL: `c.Do(ctx, http.MethodPost, "/api/v1/x", b)`.
// A call whose URL is the enclosing function's parameter makes that function a
// wrapper (`postAuth(ctx, path, req)` -> `c.Do(ctx, http.MethodPost, path, b)`),
// and a call to a function with a path-literal argument (`c.postAuth(ctx,
// "/api/v1/auth/login", req)`) is a wrapper call resolve_contracts keeps only
// when the name binds to a wrapper.

// A URL argument reduced to the route path it names, as UrlTemplate does for
// JavaScript: literal text is kept up to the query string; one leading value
// (`$baseUrl`, `c.BaseURL`) is the host and is dropped; a value filling a whole
// segment is a parameter, `{}`; the enclosing function's parameter as the tail
// marks a wrapper; anything else makes the URL unresolvable.
struct ClientUrl {
  // Whether the URL is absolute (`http.Get(url)`, Ktor's `client.get(url)`), so a
  // leading value is the host. A Go client method taking a path relative to its
  // own base (`c.Do(ctx, method, path, body)`) has no host in front: a leading
  // value there (`n.Base+"/import"`) is a path prefix this file cannot read.
  bool absolute = true;
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
  // A value this file does not read.
  void value() {
    if (in_query) {
      return;
    }
    if (path.empty() && !tail) {
      if (!absolute) {
        resolvable = false;
        return;
      }
      // The host. A second leading value (`$host$prefix/users`) may carry a path
      // prefix this file cannot see: dropping it too would mint a truncated route.
      resolvable = resolvable && !dropped_host;
      dropped_host = true;
      return;
    }
    if (!tail && path.back() == '/') {
      path += "{}";  // a whole-segment parameter: `/sessions/$id/invalidate`
      return;
    }
    resolvable = false;  // `/v1-$x`, or a value after the tail
  }
  // A call building part of the URL from runtime values (`ensureLeadingSlash(path)`,
  // `base(projectId)`): at the front it may return a path, so it is not a host.
  void built_by_call() {
    if (in_query) {
      return;
    }
    if (path.empty()) {
      resolvable = false;
      return;
    }
    value();
  }
  // A parameter of the enclosing function: after a slash it fills a segment;
  // otherwise it is the tail a wrapper appends (`"$baseUrl$path"`, `path`).
  void parameter() {
    if (in_query) {
      return;
    }
    if (tail) {
      resolvable = false;
      return;
    }
    if (!path.empty() && path.back() == '/') {
      path += "{}";
      return;
    }
    tail = true;
  }
  void finish() {
    if (path.starts_with("http://") || path.starts_with("https://")) {
      resolvable = false;  // another host, spelled out: not this repository's contract
      return;
    }
    if (!tail && (path.empty() || path.front() != '/')) {
      resolvable = false;
    }
  }
};

[[nodiscard]] std::string upper_ascii(std::string text) {
  for (auto& ch : text) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return text;
}

[[nodiscard]] std::string lower_ascii(std::string text) {
  for (auto& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return text;
}

// The receiver of `X.get(url)` is an HTTP client when its name ends in one
// (`client`, `httpClient`, `http`, `api`, `authApi`), so a `map.get("/key")` or
// a `cache.delete(key)` is never read as a request. A name that only contains
// the word is something else holding clients or HTTP data: `clients.get(key)`
// (a map), `clientRepository.get(id)`, `httpCache.get(key)`.
[[nodiscard]] bool names_http_client(std::string_view receiver) {
  const auto lower = lower_ascii(std::string(receiver));
  return lower.ends_with("client") || lower.ends_with("http") || lower.ends_with("api");
}

[[nodiscard]] TSNode named_child_of_type(const TSNode& node, std::string_view type) {
  const auto count = ts_node_named_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode child = ts_node_named_child(node, index);
    if (std::string_view(ts_node_type(child)) == type) {
      return child;
    }
  }
  return TSNode{};
}

// The http_call fact for a request, or the http_wrapper fact when the URL is
// the enclosing function's parameter. `method` is uppercase, empty when the
// call does not fix it (then the URL is left unresolved: a verb cannot be
// guessed). `client` carries a `.` for a primitive client call and is a bare
// name for a call to a (possible) wrapper.
void emit_client_call(const ExtractionContext& context, const std::string& function_scope_id, std::string client,
                      const std::string& method, ClientUrl url, std::vector<RawRelation>& out) {
  url.finish();
  if (url.tail && url.resolvable && !method.empty() && !function_scope_id.empty()) {
    out.push_back(RawRelation{
        .source_id = function_scope_id,
        .target_label = std::move(client),
        .relation = "http_wrapper",
        .context = method + " " + url.path,
        .source_file = context.source_file,
    });
    return;
  }
  // A parameter tail that is no prefix wrapper (`path+"?"+q`, a verb held in a
  // variable) leaves the request itself unresolved.
  const bool resolved = url.resolvable && !url.tail && !method.empty();
  out.push_back(RawRelation{
      .source_id = function_scope_id.empty() ? make_id(context.relative_path) : function_scope_id,
      .target_label = std::move(client),
      .relation = "http_call",
      .context = method + " " + (resolved ? url.path : std::string{}),
      .source_file = context.source_file,
  });
}

// A call to a function (not a client) with a path-literal argument: the first
// such argument is the path a wrapper appends. Method empty: the wrapper fixes it.
void emit_wrapper_call(const ExtractionContext& context, const std::string& function_scope_id, std::string callee,
                       const ClientUrl& url, std::vector<RawRelation>& out) {
  out.push_back(RawRelation{
      .source_id = function_scope_id.empty() ? make_id(context.relative_path) : function_scope_id,
      .target_label = std::move(callee),
      .relation = "http_call",
      .context = " " + url.path,
      .source_file = context.source_file,
  });
}

// The parameters of the named function a call sits in, less every name a
// lambda in between declares. A wrapper often runs its request inside one
// (`= withContext(Dispatchers.IO) { client.post("$baseUrl$path") }`, `retry(func()
// error { c.Do(ctx, method, path, body) })`): the function's `path` is still the
// tail it appends. A lambda's own parameter (`ids.map { path -> ... }`) is a
// value, not the function's argument, and shadows a function parameter so named.
[[nodiscard]] std::vector<std::string> without_shadowed(std::vector<std::string> names,
                                                        const std::vector<std::string>& shadowed) {
  std::erase_if(names, [&](const std::string& name) { return std::ranges::find(shadowed, name) != shadowed.end(); });
  return names;
}

// Kotlin: see without_shadowed. A lambda without declared parameters binds `it`.
[[nodiscard]] std::vector<std::string> kotlin_enclosing_parameters(const TSNode& node, std::string_view source) {
  std::vector<std::string> shadowed;
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    const std::string_view type = ts_node_type(ancestor);
    if (type == "lambda_literal") {
      const TSNode parameters = named_child_of_type(ancestor, "lambda_parameters");
      if (ts_node_is_null(parameters)) {
        shadowed.emplace_back("it");
        continue;
      }
      for (std::uint32_t index = 0; index < ts_node_named_child_count(parameters); ++index) {
        const TSNode parameter = ts_node_named_child(parameters, index);
        const std::string_view parameter_type = ts_node_type(parameter);
        if (parameter_type == "variable_declaration") {
          shadowed.push_back(go_node_text(named_child_of_type(parameter, "simple_identifier"), source));
        } else if (parameter_type == "multi_variable_declaration") {  // `{ (key, value) -> ... }`
          for (std::uint32_t n = 0; n < ts_node_named_child_count(parameter); ++n) {
            shadowed.push_back(
                go_node_text(named_child_of_type(ts_node_named_child(parameter, n), "simple_identifier"), source));
          }
        }
      }
      continue;
    }
    const bool anonymous = type == "anonymous_function";
    if (!anonymous && type != "function_declaration") {
      continue;
    }
    std::vector<std::string> names;
    const TSNode parameters = named_child_of_type(ancestor, "function_value_parameters");
    const auto count = ts_node_named_child_count(parameters);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode parameter = ts_node_named_child(parameters, index);
      if (std::string_view(ts_node_type(parameter)) == "parameter") {
        names.push_back(go_node_text(named_child_of_type(parameter, "simple_identifier"), source));
      }
    }
    if (anonymous) {  // `fun(path: String) { ... }` is a lambda too
      shadowed.insert(shadowed.end(), names.begin(), names.end());
      continue;
    }
    return without_shadowed(std::move(names), shadowed);
  }
  return {};
}

void kotlin_collect_url(const TSNode& node, std::string_view source, const std::vector<std::string>& parameters,
                        ClientUrl& url) {
  const std::string_view type = ts_node_type(node);
  const auto named = [&](const TSNode& identifier) {
    if (std::ranges::find(parameters, go_node_text(identifier, source)) != parameters.end()) {
      url.parameter();
    } else {
      url.value();
    }
  };
  if (type == "string_literal") {
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode part = ts_node_named_child(node, index);
      const std::string_view part_type = ts_node_type(part);
      if (part_type == "string_content") {
        url.literal(go_node_text(part, source));
      } else if (part_type == "interpolated_identifier") {
        named(part);
      } else if (part_type == "interpolated_expression") {
        const TSNode inner = ts_node_named_child(part, 0);
        const std::string_view inner_type = ts_node_is_null(inner) ? std::string_view{} : ts_node_type(inner);
        if (inner_type == "simple_identifier") {
          named(inner);
        } else if (inner_type == "call_expression") {
          url.built_by_call();
        } else {
          url.value();
        }
      }
    }
    return;
  }
  if (type == "simple_identifier") {
    named(node);
    return;
  }
  if (type == "additive_expression" && ts_node_named_child_count(node) == 2 && ts_node_child_count(node) == 3 &&
      std::string_view(ts_node_type(ts_node_child(node, 1))) == "+") {
    kotlin_collect_url(ts_node_named_child(node, 0), source, parameters, url);
    kotlin_collect_url(ts_node_named_child(node, 1), source, parameters, url);
    return;
  }
  if (type == "call_expression") {
    url.built_by_call();
    return;
  }
  url.value();
}

// The expression of a `value_argument`: its last named child (a named argument
// `path = "..."` puts the label first).
[[nodiscard]] TSNode kotlin_argument_value(const TSNode& argument) {
  const auto count = ts_node_named_child_count(argument);
  return count == 0 ? TSNode{} : ts_node_named_child(argument, count - 1);
}

// The last name of a receiver: `client`, `this.client`, `registered.client`.
[[nodiscard]] std::string kotlin_receiver_name(const TSNode& receiver, std::string_view source) {
  const std::string_view type = ts_node_type(receiver);
  if (type == "simple_identifier") {
    return go_node_text(receiver, source);
  }
  if (type != "navigation_expression") {
    return {};
  }
  const auto count = ts_node_named_child_count(receiver);
  const TSNode suffix = count == 0 ? TSNode{} : ts_node_named_child(receiver, count - 1);
  if (ts_node_is_null(suffix) || std::string_view(ts_node_type(suffix)) != "navigation_suffix") {
    return {};
  }
  return go_node_text(named_child_of_type(suffix, "simple_identifier"), source);
}

void kotlin_http_walk(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                      Fragment& /*fragment*/, std::vector<RawCall>& /*raw_calls*/, std::vector<RawRelation>& out) {
  kotlin_env_reads(node, context, function_scope_id, out);
  kotlin_header_contracts(node, context, function_scope_id, out);
  if (std::string_view(ts_node_type(node)) != "call_expression") {
    return;
  }
  const TSNode callee = ts_node_named_child(node, 0);
  const TSNode suffix = named_child_of_type(node, "call_suffix");
  const TSNode arguments = ts_node_is_null(suffix) ? TSNode{} : named_child_of_type(suffix, "value_arguments");
  if (ts_node_is_null(callee) || ts_node_is_null(arguments) || ts_node_named_child_count(arguments) == 0) {
    return;  // `restClient.post()` starts a builder chain: no URL here
  }
  const std::string_view callee_type = ts_node_type(callee);
  if (callee_type == "navigation_expression") {
    const auto count = ts_node_named_child_count(callee);
    const TSNode member = ts_node_named_child(callee, count - 1);
    if (count != 2 || std::string_view(ts_node_type(member)) != "navigation_suffix") {
      return;
    }
    const auto verb = go_node_text(named_child_of_type(member, "simple_identifier"), context.source);
    const bool request = verb == "request";
    if ((!is_http_verb(verb) || verb == "all") && !request) {
      return;
    }
    const auto receiver = kotlin_receiver_name(ts_node_named_child(callee, 0), context.source);
    if (!names_http_client(receiver)) {
      return;
    }
    ClientUrl url;
    kotlin_collect_url(kotlin_argument_value(ts_node_named_child(arguments, 0)), context.source,
                       kotlin_enclosing_parameters(node, context.source), url);
    // `client.request(url) { method = ... }` sets its verb in the builder, which
    // this does not read: the call counts, unresolved.
    emit_client_call(context, function_scope_id, receiver + "." + verb, request ? std::string{} : upper_ascii(verb),
                     std::move(url), out);
    return;
  }
  if (callee_type != "simple_identifier") {
    return;
  }
  // `postLoginOutcome(path = "/api/v1/auth/login", ...)`, `decide(token,
  // "$baseUrl/api/v1/x/$id/approve")`: a possible wrapper call.
  const auto count = ts_node_named_child_count(arguments);
  for (std::uint32_t index = 0; index < count; ++index) {
    const TSNode value = kotlin_argument_value(ts_node_named_child(arguments, index));
    if (ts_node_is_null(value) || std::string_view(ts_node_type(value)) != "string_literal") {
      continue;
    }
    ClientUrl url;
    kotlin_collect_url(value, context.source, {}, url);
    url.finish();
    if (url.resolvable && url.path.find_first_not_of('/') != std::string::npos) {
      emit_wrapper_call(context, function_scope_id, go_node_text(callee, context.source), url, out);
      return;  // the first path-like string argument is the path
    }
  }
}

[[nodiscard]] LanguageConfig kotlin_config() {
  LanguageConfig config{
      .name = "kotlin",
      .grammar_name = "tree-sitter-kotlin",
      .extensions = {".kt", ".kts"},
      // An interface is a `class_declaration` with an `interface` modifier in
      // this grammar -- there is no separate interface_declaration node.
      .class_node_types = {"class_declaration", "object_declaration"},
      .function_node_types = {"function_declaration"},
      .call_node_types = {"call_expression"},
      // The grammar exposes no name/callee fields, so naming is positional. Kotlin
      // imports are package-qualified and decoupled from file layout (like Rust's
      // `use`), so import resolution is a non-goal and no import_handler is set.
  };
  config.resolve_callee_name = kotlin_callee_name;
  config.resolve_function_name = kotlin_symbol_name;
  config.relation_handler = kotlin_relation_handler;
  config.extra_walk = kotlin_http_walk;
  return config;
}

[[nodiscard]] LanguageConfig scala_config() {
  return LanguageConfig{
      .name = "scala",
      .grammar_name = "tree-sitter-scala",
      .extensions = {".scala", ".sc"},
      .class_node_types = {"class_definition", "object_definition", "trait_definition"},
      .function_node_types = {"function_definition"},
      .import_node_types = {"import_declaration"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
  };
}

[[nodiscard]] std::string go_node_text(const TSNode& node, std::string_view source) {
  if (ts_node_is_null(node)) return {};
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  if (start >= end || end > source.size()) {
    return {};
  }
  return std::string(source.substr(start, end - start));
}

// `import "net/http"` / grouped `import ( alias "pkg/path" )`: each import_spec's
// quoted path becomes a module stub node + a file -> module `imports` edge, the
// same shape cpp_import_handler emits. resolve_imports matches the spec against
// project files by path suffix; stdlib and external module paths match nothing
// and are dropped, leaving no dangling edge.
void go_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  if (std::string_view(ts_node_type(node)) != "import_spec") {
    return;
  }
  const auto path = ts_node_child_by_field_name(node, "path", 4);
  if (ts_node_is_null(path)) {
    return;
  }
  auto spec = go_node_text(path, context.source);
  if (spec.size() >= 2 && (spec.front() == '"' || spec.front() == '`')) {
    spec = spec.substr(1, spec.size() - 2);  // strip the surrounding "" or ``
  }
  if (spec.empty()) {
    return;
  }

  const auto module_id = make_id(spec);
  fragment.nodes.push_back(Node{
      .id = module_id,
      .label = spec,
      .source_location = SourceLocation{.start_line = 1, .end_line = 1},
      .kind = "module",
      .confidence = Confidence::Extracted,
      .properties = {{"import_path", spec}},
  });
  fragment.edges.push_back(Edge{
      .source = make_id(context.relative_path),
      .target = module_id,
      .relation = "imports",
      .confidence = Confidence::Extracted,
  });
}


// Deepest type_identifier under a (possibly pointer-wrapped, parenthesized)
// receiver type: `(r *Route)` -> "Route".
[[nodiscard]] std::string go_receiver_type_name(const TSNode& node, const ExtractionContext& context) {
  if (std::string_view(ts_node_type(node)) == "type_identifier") {
    return go_node_text(node, context.source);
  }
  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    auto found = go_receiver_type_name(ts_node_child(node, index), context);
    if (!found.empty()) {
      return found;
    }
  }
  return {};
}

// Binds each Go method to its receiver type: `func (r *Route) Match(...)` emits
// a `method_of` fact (method -> "Route") that resolve_raw_relations turns into
// an edge when the type is declared in the same file (Go's common layout) or an
// imported one. Interface-dispatch resolution reads these to compute per-type
// method sets.
void go_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                         std::vector<RawRelation>& raw_relations) {
  if (std::string_view(ts_node_type(node)) != "method_declaration") {
    return;
  }
  const auto receiver = ts_node_child_by_field_name(node, "receiver", 8);
  if (ts_node_is_null(receiver)) {
    return;
  }
  auto type_name = go_receiver_type_name(receiver, context);
  if (type_name.empty()) {
    return;
  }
  raw_relations.push_back(RawRelation{
      .source_id = node_id,
      .target_label = std::move(type_name),
      .relation = "method_of",
      .context = "receiver",
      .source_file = context.source_file,
      .allow_same_file = true,
  });
}

// Go: the parameter names of the enclosing function or method, less those a
// function literal in between declares (see without_shadowed).
[[nodiscard]] std::vector<std::string> go_enclosing_parameters(const TSNode& node, std::string_view source) {
  std::vector<std::string> shadowed;
  for (TSNode ancestor = ts_node_parent(node); !ts_node_is_null(ancestor); ancestor = ts_node_parent(ancestor)) {
    const std::string_view type = ts_node_type(ancestor);
    const bool literal = type == "func_literal";
    if (!literal && type != "function_declaration" && type != "method_declaration") {
      continue;
    }
    std::vector<std::string> names;
    const TSNode parameters = ts_node_child_by_field_name(ancestor, "parameters", 10);
    const auto count = ts_node_named_child_count(parameters);
    for (std::uint32_t index = 0; index < count; ++index) {
      const TSNode declaration = ts_node_named_child(parameters, index);
      const auto names_count = ts_node_named_child_count(declaration);
      for (std::uint32_t n = 0; n < names_count; ++n) {
        const TSNode child = ts_node_named_child(declaration, n);
        if (std::string_view(ts_node_type(child)) == "identifier") {
          names.push_back(go_node_text(child, source));  // `method, path string` names two
        }
      }
    }
    if (literal) {
      shadowed.insert(shadowed.end(), names.begin(), names.end());
      continue;
    }
    return without_shadowed(std::move(names), shadowed);
  }
  return {};
}

// The text of a Go string literal, or nullopt for anything else.
[[nodiscard]] std::optional<std::string> go_string_value(const TSNode& node, std::string_view source) {
  const std::string_view type = ts_node_type(node);
  if (type != "interpreted_string_literal" && type != "raw_string_literal") {
    return std::nullopt;
  }
  const auto text = go_node_text(node, source);
  return text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string{};
}

void go_collect_url(const TSNode& node, std::string_view source, const std::vector<std::string>& parameters,
                    ClientUrl& url) {
  const std::string_view type = ts_node_type(node);
  if (const auto text = go_string_value(node, source)) {
    url.literal(*text);
    return;
  }
  if (type == "identifier") {
    if (std::ranges::find(parameters, go_node_text(node, source)) != parameters.end()) {
      url.parameter();
    } else {
      url.value();
    }
    return;
  }
  if (type == "binary_expression" &&
      go_node_text(ts_node_child_by_field_name(node, "operator", 8), source) == "+") {
    go_collect_url(ts_node_child_by_field_name(node, "left", 4), source, parameters, url);
    go_collect_url(ts_node_child_by_field_name(node, "right", 5), source, parameters, url);
    return;
  }
  if (type == "parenthesized_expression" && ts_node_named_child_count(node) == 1) {
    go_collect_url(ts_node_named_child(node, 0), source, parameters, url);
    return;
  }
  if (type == "call_expression") {
    url.built_by_call();
    return;
  }
  url.value();
}

// The HTTP method an argument names: `http.MethodPost` or a literal "POST".
// Empty when it names none.
[[nodiscard]] std::string go_method_argument(const TSNode& argument, std::string_view source) {
  std::string name;
  if (std::string_view(ts_node_type(argument)) == "selector_expression") {
    if (go_node_text(ts_node_child_by_field_name(argument, "operand", 7), source) != "http") {
      return {};
    }
    const auto field = go_node_text(ts_node_child_by_field_name(argument, "field", 5), source);
    if (!field.starts_with("Method")) {
      return {};
    }
    name = field.substr(6);
  } else if (const auto text = go_string_value(argument, source); text && *text == upper_ascii(*text)) {
    name = *text;
  }
  const auto lower = lower_ascii(name);
  return is_http_verb(lower) && lower != "all" ? upper_ascii(name) : std::string{};
}

// Whether an argument is a context.Context: `ctx`, `reqCtx`, `s.ctx`,
// `cmd.Context()`, `r.Context()`, `context.Background()`.
[[nodiscard]] bool go_context_argument(const TSNode& argument, std::string_view source) {
  const std::string_view type = ts_node_type(argument);
  const auto names_context = [](std::string name) {
    name = lower_ascii(std::move(name));
    return name.ends_with("ctx") || name == "context";
  };
  if (type == "identifier") {
    return names_context(go_node_text(argument, source));
  }
  if (type == "selector_expression") {
    return names_context(go_node_text(ts_node_child_by_field_name(argument, "field", 5), source));
  }
  if (type != "call_expression") {
    return false;
  }
  const TSNode function = ts_node_child_by_field_name(argument, "function", 8);
  if (std::string_view(ts_node_type(function)) != "selector_expression") {
    return false;
  }
  return go_node_text(ts_node_child_by_field_name(function, "field", 5), source) == "Context" ||
         go_node_text(ts_node_child_by_field_name(function, "operand", 7), source) == "context";
}

// Whether the argument after a verb can be a URL: a string literal only when it
// reads as a path or an absolute URL (`"rev-parse"` is a git subcommand); any
// expression a URL may be held in or built by (`path`, `n.Base+"/import"`,
// `n.itemPath(id)`); nothing else (`nil`, a number, a composite literal).
[[nodiscard]] bool go_url_like(const TSNode& argument, std::string_view source) {
  const std::string_view type = ts_node_type(argument);
  if (const auto text = go_string_value(argument, source)) {
    return text->starts_with('/') || text->starts_with("http://") || text->starts_with("https://");
  }
  if (type == "binary_expression") {
    return go_url_like(ts_node_child_by_field_name(argument, "left", 4), source);
  }
  if (type == "parenthesized_expression" && ts_node_named_child_count(argument) == 1) {
    return go_url_like(ts_node_named_child(argument, 0), source);
  }
  return type == "identifier" || type == "selector_expression" || type == "call_expression" ||
         type == "index_expression";
}

void go_http_walk(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                  std::vector<RawRelation>& out) {
  if (std::string_view(ts_node_type(node)) != "call_expression") {
    return;
  }
  const TSNode callee = ts_node_child_by_field_name(node, "function", 8);
  const TSNode arguments = ts_node_child_by_field_name(node, "arguments", 9);
  if (ts_node_is_null(callee) || ts_node_is_null(arguments)) {
    return;
  }
  std::vector<TSNode> args;
  for (std::uint32_t index = 0; index < ts_node_named_child_count(arguments); ++index) {
    const TSNode argument = ts_node_named_child(arguments, index);
    if (std::string_view(ts_node_type(argument)) != "comment") {
      args.push_back(argument);
    }
  }
  if (args.empty()) {
    return;
  }
  std::string package;
  std::string name;
  const std::string_view callee_type = ts_node_type(callee);
  if (callee_type == "selector_expression") {
    const TSNode operand = ts_node_child_by_field_name(callee, "operand", 7);
    if (std::string_view(ts_node_type(operand)) == "identifier") {
      package = go_node_text(operand, context.source);
    } else if (std::string_view(ts_node_type(operand)) == "selector_expression") {
      package = go_node_text(ts_node_child_by_field_name(operand, "field", 5), context.source);  // `c.http.Do`
    }
    name = go_node_text(ts_node_child_by_field_name(callee, "field", 5), context.source);
  } else if (callee_type == "identifier") {
    name = go_node_text(callee, context.source);
  } else {
    return;
  }
  // httptest builds the server-side request a handler test feeds its handler
  // (`httptest.NewRequest("GET", "/api/v1/users", nil)`): nothing is sent.
  if (package == "httptest") {
    return;
  }
  // A handler argument: a route registration (`r.Get("/x", func(w, r) {...})`)
  // or a callback API, never a request -- the JavaScript extractor's rule.
  if (std::ranges::any_of(args, [](const TSNode& argument) {
        return std::string_view(ts_node_type(argument)) == "func_literal";
      })) {
    return;
  }
  const auto client = package.empty() ? name : package + "." + name;
  const auto parameters = go_enclosing_parameters(node, context.source);
  const auto request = [&](std::string method, const TSNode& url_argument, bool absolute) {
    ClientUrl url{.absolute = absolute};
    go_collect_url(url_argument, context.source, parameters, url);
    emit_client_call(context, function_scope_id, client, method, std::move(url), out);
  };
  // net/http's own entry points.
  if (package == "http") {
    static constexpr std::pair<std::string_view, std::string_view> kShorthands[] = {
        {"Get", "GET"}, {"Head", "HEAD"}, {"Post", "POST"}, {"PostForm", "POST"}};
    for (const auto& [function, method] : kShorthands) {
      if (name == function) {
        request(std::string(method), args[0], true);
        return;
      }
    }
    if (name == "NewRequest" || name == "NewRequestWithContext") {
      const std::size_t method_index = name == "NewRequest" ? 0 : 1;
      if (args.size() > method_index + 1) {
        request(go_method_argument(args[method_index], context.source), args[method_index + 1], true);
      }
      return;
    }
  }
  // A client method taking a context, the verb and then the URL: `c.Do(ctx,
  // http.MethodGet, "/api/v1/auth/me", nil)`, `s.Client.Mutate(cmd.Context(),
  // "PATCH", path, nil)`. The receiver's name says nothing here (`c`, `scoped`
  // are clients; `r`, `e`, `router` are routers), so the call's shape decides:
  // a Go request carries its context.Context first, as net/http's own
  // NewRequestWithContext does, while a route registration (`r.Handle(
  // http.MethodGet, "/x", h)`, echo's `e.Add("DELETE", ...)`), a test helper
  // (`httpmock.RegisterResponder("GET", ...)`), an assertion on a request's
  // method (`assert.Equal(t, http.MethodPost, r.Method)`) or a log line takes
  // none. The argument after the verb must read as a URL, so `git.Run(ctx,
  // "HEAD", "--quiet")` is no request either.
  for (std::size_t index = 0; index + 2 < args.size(); ++index) {
    if (!go_context_argument(args[index], context.source)) {
      continue;
    }
    if (auto method = go_method_argument(args[index + 1], context.source);
        !method.empty() && go_url_like(args[index + 2], context.source)) {
      request(std::move(method), args[index + 2], false);
      return;
    }
  }
  // `c.postAuth(ctx, "/api/v1/auth/login", req)`: a possible wrapper call, by the
  // bare name resolve_contracts binds in this file.
  for (const auto& argument : args) {
    const auto text = go_string_value(argument, context.source);
    if (!text) {
      continue;
    }
    ClientUrl url;
    url.literal(*text);
    url.finish();
    if (url.resolvable && url.path.find_first_not_of('/') != std::string::npos) {
      emit_wrapper_call(context, function_scope_id, name, url, out);
      return;  // the first path-like string argument is the path
    }
  }
}

// Materializes Go interface method sets: each `method_elem` of an
// `interface_type` becomes a function node (tagged interface_method) owned by
// the interface's type node via a `method` edge, so dispatch resolution can
// see what an interface promises. The node id is namespaced — an interface
// method is a contract entry, never the same node as an implementation.
void go_extra_walk(const TSNode& node, const ExtractionContext& context,
                   const std::string& function_scope_id, Fragment& fragment,
                   std::vector<RawCall>& raw_calls, std::vector<RawRelation>& raw_relations) {
  (void)raw_calls;
  go_http_walk(node, context, function_scope_id, raw_relations);
  go_env_reads(node, context, function_scope_id, raw_relations);
  go_header_contracts(node, context, function_scope_id, raw_relations);
  if (std::string_view(ts_node_type(node)) != "type_spec") {
    return;
  }
  const auto type_field = ts_node_child_by_field_name(node, "type", 4);
  if (ts_node_is_null(type_field) || std::string_view(ts_node_type(type_field)) != "interface_type") {
    return;
  }
  const auto name_field = ts_node_child_by_field_name(node, "name", 4);
  if (ts_node_is_null(name_field)) {
    return;
  }
  const auto interface_name = go_node_text(name_field, context.source);
  if (interface_name.empty()) {
    return;
  }
  const auto interface_id = make_id(context.relative_path + ":" + interface_name);

  const auto child_count = ts_node_named_child_count(type_field);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const auto elem = ts_node_named_child(type_field, index);
    if (std::string_view(ts_node_type(elem)) != "method_elem") {
      continue;  // embedded interfaces are a follow-up
    }
    const auto method_name_node = ts_node_child_by_field_name(elem, "name", 4);
    if (ts_node_is_null(method_name_node)) {
      continue;
    }
    const auto method_name = go_node_text(method_name_node, context.source);
    if (method_name.empty()) {
      continue;
    }
    const auto start = ts_node_start_point(elem);
    const auto end = ts_node_end_point(elem);
    fragment.nodes.push_back(Node{
        .id = make_id("iface-method:" + context.relative_path + ":" + interface_name + ":" + method_name),
        .label = method_name,
        .source_file = context.source_file,
        .source_location = SourceLocation{.start_line = start.row + 1,
                                          .start_column = start.column,
                                          .end_line = end.row + 1,
                                          .end_column = end.column},
        .kind = "function",
        .confidence = Confidence::Extracted,
        .properties = {{"method", "true"}, {"interface_method", "true"}},
    });
    fragment.edges.push_back(Edge{
        .source = interface_id,
        .target = fragment.nodes.back().id,
        .relation = "method",
        .confidence = Confidence::Extracted,
    });
  }
}

[[nodiscard]] LanguageConfig go_config() {
  LanguageConfig config{
      .name = "go",
      .grammar_name = "tree-sitter-go",
      .extensions = {".go"},
      // Named types (`type Server struct {...}`, `type Handler interface {...}`,
      // aliases) are all declared through type_spec / type_alias; they become
      // "type" nodes rather than guessing class-ness per underlying type.
      .function_node_types = {"function_declaration", "method_declaration"},
      .method_node_types = {"method_declaration"},
      .type_node_types = {"type_spec", "type_alias"},
      .import_node_types = {"import_spec"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
      // `pkg.Func()` / `recv.Method()` targets are selector_expressions; record
      // the bare field name as a member call so resolution stays same-file (the
      // receiver/package is not resolved by a project-wide name guess).
      .call_member_node_types = {"selector_expression"},
      .call_member_field = "field",
      .resolve_callee_name = cpp_callee_name,
  };
  config.import_handler = go_import_handler;
  config.relation_handler = go_relation_handler;
  config.extract_members = true;
  config.member_handler = go_member_handler;
  config.extra_walk = go_extra_walk;
  return config;
}

// Rust `::`-qualified and turbofish callees are shapes cpp_callee_name does not
// know (callee_leaf_name only descends the C-family node kinds), so reusing it
// would silently drop every `Type::assoc()` and `foo::<T>()` call. Descend the
// Rust-specific wrappers to the leaf identifier instead:
//   scoped_identifier -> its `name` (Type::assoc / path::to::fn)
//   generic_function  -> its `function` (turbofish foo::<T>())
//   field_expression  -> its `field` (x.method::<T>())
// and stop at identifier / field_identifier. Anything else yields no name (the
// call is dropped rather than guessed).
[[nodiscard]] std::string rust_callee_name(const TSNode& node, const ExtractionContext& context) {
  TSNode cur = node;
  while (!ts_node_is_null(cur)) {
    const std::string_view type = ts_node_type(cur);
    if (type == "identifier" || type == "field_identifier") {
      return go_node_text(cur, context.source);
    }
    if (type == "scoped_identifier") {
      cur = ts_node_child_by_field_name(cur, "name", 4);
      continue;
    }
    if (type == "generic_function") {
      cur = ts_node_child_by_field_name(cur, "function", 8);
      continue;
    }
    if (type == "field_expression") {
      cur = ts_node_child_by_field_name(cur, "field", 5);
      continue;
    }
    return {};
  }
  return {};
}

// --- Rust `use` imports ------------------------------------------------------
// A `use` path is `::`-delimited and decoupled from file layout (`a/b.rs` vs
// `a/b/mod.rs`), and the syntax alone cannot tell whether the last segment names
// a module or an item declared in one. Extraction therefore records one stub per
// imported leaf -- the full `/`-joined path, the original (pre-alias) name, and
// a `module_layout=rust` marker -- and resolve_imports owns the layout decision:
// `<path>.rs` / `<path>/mod.rs` first, then the parent path as the module with
// the leaf as a declared item. Glob (`use a::b::*`) and `{self}` leaves name the
// module itself and are emitted as `module` stubs. Every stub is consumed by
// resolve_imports (remapped or dropped), so the marker never reaches an export.
// Leading `crate::`/`self::`/`super::` segments only position the path within
// the project and are stripped before unique-suffix resolution.

[[nodiscard]] bool rust_plain_use_path(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  return std::ranges::all_of(text, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == ':';
  });
}

// Append the `::`-split segments of a path-shaped node. Returns false for
// anything that is not a plain path -- a bracketed `<T as Trait>` or generic
// path is dropped rather than guessed.
[[nodiscard]] bool rust_append_use_path(
    const TSNode& node, const ExtractionContext& context, std::vector<std::string>& segments) {
  const auto text = go_node_text(node, context.source);
  if (!rust_plain_use_path(text)) {
    return false;
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto pos = text.find("::", start);
    auto segment = text.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
    if (!segment.empty()) {
      segments.push_back(std::move(segment));
    }
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 2;
  }
  return true;
}

void rust_emit_use_stub(
    std::vector<std::string> segments, bool module_only, bool is_reexport,
    const ExtractionContext& context, Fragment& fragment) {
  std::size_t first = 0;
  while (first < segments.size() &&
         (segments[first] == "crate" || segments[first] == "self" || segments[first] == "super")) {
    ++first;
  }
  bool as_module = module_only;
  if (segments.size() > first && segments.back() == "self") {
    segments.pop_back();  // `use a::b::{self, ...}`: the self leaf IS module a::b
    as_module = true;
  }
  if (first >= segments.size()) {
    return;  // `use crate::*;` and friends: nothing project-resolvable remains
  }
  std::string joined = segments[first];
  for (std::size_t index = first + 1; index < segments.size(); ++index) {
    joined += "/" + segments[index];
  }
  const auto label = as_module ? joined : segments.back();
  // The id carries path AND label so an item stub and a module stub over the
  // same path never collide across fragments (first-occurrence-wins in merge
  // would otherwise pick one kind nondeterministically).
  const auto stub_id = make_id("rust_use:" + joined + ":" + label);
  if (!node_id_taken(fragment, stub_id)) {
    Node stub{
        .id = stub_id,
        .label = label,
        .source_location = SourceLocation{.start_line = 1, .end_line = 1},
        .kind = as_module ? "module" : "import",
        .confidence = Confidence::Extracted,
        .properties = {{"import_path", joined}, {"module_layout", "rust"}},
    };
    // `pub use a::b::Item` re-exports Item from this file: a consumer that spells
    // `this_module::Item` reaches the real definition only by following the
    // re-export chain (issue #60 -- tokio-util's tests reach a changed
    // `tokio::task::LocalSet` through `pub use local::LocalSet` in task/mod.rs).
    // Mark the stub so resolve_imports can follow it; a private `use` stays an
    // internal import that no outside consumer resolves through.
    if (is_reexport) {
      stub.properties.emplace("reexport", "true");
    }
    fragment.nodes.push_back(std::move(stub));
  }
  fragment.edges.push_back(Edge{
      .source = make_id(context.relative_path),
      .target = stub_id,
      .relation = is_reexport ? "re_exports" : "imports",
      .confidence = Confidence::Extracted,
  });
}

void rust_walk_use_tree(
    const TSNode& node,
    const ExtractionContext& context,
    Fragment& fragment,
    const std::vector<std::string>& prefix,
    bool is_reexport) {
  const std::string_view type = ts_node_type(node);
  if (type == "identifier" || type == "scoped_identifier" || type == "crate" ||
      type == "self" || type == "super") {
    auto segments = prefix;
    if (rust_append_use_path(node, context, segments)) {
      rust_emit_use_stub(std::move(segments), false, is_reexport, context, fragment);
    }
    return;
  }
  if (type == "use_as_clause") {
    // Resolution goes through the ORIGINAL name; the alias never becomes a node
    // (resolve_imports remaps by the name declared in the target file).
    const auto path = ts_node_child_by_field_name(node, "path", 4);
    if (!ts_node_is_null(path)) {
      rust_walk_use_tree(path, context, fragment, prefix, is_reexport);
    }
    return;
  }
  if (type == "scoped_use_list") {
    auto segments = prefix;
    const auto path = ts_node_child_by_field_name(node, "path", 4);
    if (!ts_node_is_null(path) && !rust_append_use_path(path, context, segments)) {
      return;
    }
    const auto list = ts_node_child_by_field_name(node, "list", 4);
    if (!ts_node_is_null(list)) {
      rust_walk_use_tree(list, context, fragment, segments, is_reexport);
    }
    return;
  }
  if (type == "use_list") {
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      rust_walk_use_tree(ts_node_named_child(node, index), context, fragment, prefix, is_reexport);
    }
    return;
  }
  if (type == "use_wildcard") {
    auto segments = prefix;
    if (ts_node_named_child_count(node) > 0 &&
        !rust_append_use_path(ts_node_named_child(node, 0), context, segments)) {
      return;
    }
    rust_emit_use_stub(std::move(segments), true, is_reexport, context, fragment);
    return;
  }
  // metavariable and anything else: dropped rather than guessed.
}

void rust_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  const std::string_view type = ts_node_type(node);
  if (type == "mod_item") {
    // A bodyless `mod math;` (or `pub mod math;`) declares that another file is
    // a child module of this one — the only edge tying `src/math.rs` into the
    // module tree. An inline `mod x { ... }` carries its body and needs none.
    // The stub's path is directory-qualified (Cargo's layout rules: a crate
    // root or mod.rs anchors its own directory, any other file anchors a
    // directory named after itself), so resolution is exact rather than a
    // project-wide suffix guess.
    if (!ts_node_is_null(ts_node_child_by_field_name(node, "body", 4))) {
      return;
    }
    const auto name_node = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name_node)) {
      return;
    }
    const auto name = go_node_text(name_node, context.source);
    if (name.empty()) {
      return;
    }
    const std::filesystem::path source(context.source_file);
    const auto stem = source.stem().string();
    auto base = source.parent_path();
    if (stem != "lib" && stem != "main" && stem != "mod") {
      base /= stem;
    }
    rust_emit_use_stub({(base / name).generic_string()}, /*module_only=*/true,
                       /*is_reexport=*/false, context, fragment);
    return;
  }
  if (type != "use_declaration") {
    return;
  }
  const auto argument = ts_node_child_by_field_name(node, "argument", 8);
  if (ts_node_is_null(argument)) {
    return;
  }
  // A leading `visibility_modifier` (`pub`, `pub(crate)`, `pub(super)`) makes the
  // use a re-export: the name becomes reachable through this module's path, so
  // resolution must be able to follow it to the real definition. A bare `use` is
  // a private, internal-only import.
  bool is_reexport = false;
  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    if (std::string_view(ts_node_type(ts_node_child(node, index))) == "visibility_modifier") {
      is_reexport = true;
      break;
    }
  }
  rust_walk_use_tree(argument, context, fragment, {}, is_reexport);
}

// A `function_item` is a method exactly when it sits in an impl block
// (function_item -> declaration_list -> impl_item); the node type alone cannot
// tell it from a free function, which is why Rust needs the context predicate
// rather than Go's method_node_types.
[[nodiscard]] bool rust_is_impl_method(const TSNode& node) {
  if (std::string_view(ts_node_type(node)) != "function_item") {
    return false;
  }
  const TSNode list = ts_node_parent(node);
  if (ts_node_is_null(list) || std::string_view(ts_node_type(list)) != "declaration_list") {
    return false;
  }
  const TSNode impl = ts_node_parent(list);
  return !ts_node_is_null(impl) && std::string_view(ts_node_type(impl)) == "impl_item";
}

// Binds each impl-block method to its self type: `impl Counter { fn bump.. }`
// emits a `method_of` fact (bump -> "Counter"), exactly what go_relation_handler
// emits for receiver syntax. `impl<T> Foo<T>` reduces to Foo (deepest
// type_identifier, shared with Go's receiver walk); `impl Matcher for Router`
// binds to Router — the `type` field is the implementing type, the `trait`
// field is the contract and is handled by trait materialization instead.
void rust_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                           std::vector<RawRelation>& raw_relations) {
  if (!rust_is_impl_method(node)) {
    return;
  }
  const TSNode impl = ts_node_parent(ts_node_parent(node));
  const auto type_field = ts_node_child_by_field_name(impl, "type", 4);
  if (ts_node_is_null(type_field)) {
    return;
  }
  auto type_name = go_receiver_type_name(type_field, context);
  if (type_name.empty()) {
    return;  // impl for a primitive or non-nominal type: nothing to bind to
  }
  raw_relations.push_back(RawRelation{
      .source_id = node_id,
      .target_label = std::move(type_name),
      .relation = "method_of",
      .context = "impl",
      .source_file = context.source_file,
      .allow_same_file = true,
  });
  // `impl AsyncRead for DuplexStream`: the `trait` field names the contract this
  // method satisfies. Name-only dispatch resolution (resolve_interface_dispatch)
  // only links a contract to a type when the trait's whole method-name set is a
  // subset of the type's -- which fails for traits with default/provided methods
  // the impl does not override (AsyncRead, the async ext-trait plumbing in #60).
  // Emit the declared trait as an `impl_trait` fact so dispatch resolution can
  // bind this exact method to its contract regardless of the subset check, scoped
  // by the trait the impl actually names. Third-party traits (std, other crates)
  // stay unresolved -- resolve_raw_relations only binds a trait the file can see.
  const auto trait_field = ts_node_child_by_field_name(impl, "trait", 5);
  if (!ts_node_is_null(trait_field)) {
    auto trait_name = go_receiver_type_name(trait_field, context);
    if (!trait_name.empty()) {
      raw_relations.push_back(RawRelation{
          .source_id = node_id,
          .target_label = std::move(trait_name),
          .relation = "impl_trait",
          .context = "impl",
          .source_file = context.source_file,
          .allow_same_file = true,
      });
    }
  }
}

// Calls inside a macro invocation are invisible to the call_expression walk:
// tree-sitter-rust exposes macro arguments as an opaque token_tree, so
// `add(1, 2)` inside `assert_eq!` is never a call_expression. Rust test bodies
// are dominated by assertion macros (issue #58: clap has 2,516 assert*! call
// sites across 1,246 #[test] functions), so without this scan most test calls
// do not exist. Recognize call shapes by token sequence: `ident (…)` is a
// plain call, `. ident (…)` a member call, `:: ident (…)` a scoped call
// reduced to its leaf (the same reduction rust_callee_name applies outside
// macros). `ident !` is a nested macro name, not a call; `ident {…}` /
// `ident […]` are struct-literal / index shapes and are skipped. Nested token
// trees (including nested macro bodies) are scanned recursively.
void rust_scan_macro_tokens(const TSNode& token_tree, const ExtractionContext& context,
                            const std::string& caller_id, std::vector<RawCall>& raw_calls) {
  const auto child_count = ts_node_child_count(token_tree);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const auto child = ts_node_child(token_tree, index);
    const std::string_view type = ts_node_type(child);
    if (type == "token_tree") {
      rust_scan_macro_tokens(child, context, caller_id, raw_calls);
      continue;
    }
    if (type != "identifier" || index + 1 >= child_count) {
      continue;
    }
    const auto next = ts_node_child(token_tree, index + 1);
    if (std::string_view(ts_node_type(next)) != "token_tree") {
      continue;
    }
    const auto delimiter = ts_node_child(next, 0);
    if (ts_node_is_null(delimiter) || std::string_view(ts_node_type(delimiter)) != "(") {
      continue;
    }
    bool is_member_call = false;
    if (index > 0 &&
        std::string_view(ts_node_type(ts_node_child(token_tree, index - 1))) == ".") {
      is_member_call = true;
    }
    auto label = go_node_text(child, context.source);
    if (label.empty()) {
      continue;
    }
    const auto start = ts_node_start_point(child);
    const auto end = ts_node_end_point(child);
    raw_calls.push_back(RawCall{
        .caller_id = caller_id,
        .callee_label = std::move(label),
        .source_file = context.source_file,
        .source_location = SourceLocation{.start_line = start.row + 1,
                                          .start_column = start.column,
                                          .end_line = end.row + 1,
                                          .end_column = end.column},
        .is_member_call = is_member_call,
    });
  }
}

// Materializes Rust trait method sets, the exact mirror of go_extra_walk's
// interface handling: each method a trait declares (function_signature_item, or
// function_item for a defaulted method) becomes a contract node (tagged
// interface_method) owned by the trait's type node via a `method` edge, so
// dispatch resolution can see what the trait promises. Also dispatches macro
// bodies to the token scan above — both jobs need a hook outside the
// allowlist-driven walk, so they share the one extra_walk slot.
void rust_extra_walk(const TSNode& node, const ExtractionContext& context,
                     const std::string& function_scope_id, Fragment& fragment,
                     std::vector<RawCall>& raw_calls, std::vector<RawRelation>&) {
  const std::string_view type = ts_node_type(node);
  if (type == "macro_invocation") {
    // Same rule as call_expression extraction: a macro at file/type scope has
    // no enclosing function, so its calls have no caller and are dropped.
    if (function_scope_id.empty()) {
      return;
    }
    const auto child_count = ts_node_child_count(node);
    for (std::uint32_t index = 0; index < child_count; ++index) {
      const auto child = ts_node_child(node, index);
      if (std::string_view(ts_node_type(child)) == "token_tree") {
        rust_scan_macro_tokens(child, context, function_scope_id, raw_calls);
      }
    }
    return;
  }
  if (type != "trait_item") {
    return;
  }
  const auto name_field = ts_node_child_by_field_name(node, "name", 4);
  if (ts_node_is_null(name_field)) {
    return;
  }
  const auto trait_name = go_node_text(name_field, context.source);
  if (trait_name.empty()) {
    return;
  }
  const auto body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(body)) {
    return;
  }
  const auto trait_id = make_id(context.relative_path + ":" + trait_name);
  const auto child_count = ts_node_named_child_count(body);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const auto elem = ts_node_named_child(body, index);
    const std::string_view elem_type = ts_node_type(elem);
    if (elem_type != "function_signature_item" && elem_type != "function_item") {
      continue;
    }
    const auto method_name_node = ts_node_child_by_field_name(elem, "name", 4);
    if (ts_node_is_null(method_name_node)) {
      continue;
    }
    const auto method_name = go_node_text(method_name_node, context.source);
    if (method_name.empty()) {
      continue;
    }
    const auto start = ts_node_start_point(elem);
    const auto end = ts_node_end_point(elem);
    fragment.nodes.push_back(Node{
        .id = make_id("trait-method:" + context.relative_path + ":" + trait_name + ":" + method_name),
        .label = method_name,
        .source_file = context.source_file,
        .source_location = SourceLocation{.start_line = start.row + 1,
                                          .start_column = start.column,
                                          .end_line = end.row + 1,
                                          .end_column = end.column},
        .kind = "function",
        .confidence = Confidence::Extracted,
        .properties = {{"method", "true"}, {"interface_method", "true"}},
    });
    fragment.edges.push_back(Edge{
        .source = trait_id,
        .target = fragment.nodes.back().id,
        .relation = "method",
        .confidence = Confidence::Extracted,
    });
  }
}

// Skip a string literal, char literal, or comment starting at `pos` (returns the
// index just past it); otherwise returns pos+1. Keeps the cfg-macro scan and its
// brace matcher from tripping on a `{`/`}` inside `"..."`, `'{'`, or a comment.
// A leading `'` is a char literal only when it closes within a couple of chars
// (`'x'`, `'\n'`); a bare `'a` is a lifetime, skipped as one character.
[[nodiscard]] std::size_t rust_skip_inert(std::string_view s, std::size_t pos) {
  const auto n = s.size();
  const char c = s[pos];
  if (c == '/' && pos + 1 < n && s[pos + 1] == '/') {
    std::size_t p = pos + 2;
    while (p < n && s[p] != '\n') ++p;
    return p;
  }
  if (c == '/' && pos + 1 < n && s[pos + 1] == '*') {
    std::size_t p = pos + 2;
    while (p + 1 < n && !(s[p] == '*' && s[p + 1] == '/')) ++p;
    return std::min(n, p + 2);
  }
  if (c == '"') {
    std::size_t p = pos + 1;
    while (p < n && s[p] != '"') {
      if (s[p] == '\\') ++p;
      ++p;
    }
    return std::min(n, p + 1);
  }
  if (c == '\'') {
    const bool char_lit =
        (pos + 1 < n && s[pos + 1] == '\\') || (pos + 2 < n && s[pos + 2] == '\'');
    if (char_lit) {
      std::size_t p = pos + 1;
      if (p < n && s[p] == '\\') ++p;
      ++p;                       // the char
      if (p < n && s[p] == '\'') ++p;
      return p;
    }
    return pos + 1;              // a lifetime `'a`
  }
  return pos + 1;
}

// Blank `cfg_*! { ... }` item-wrapper macros — tokio's `cfg_rt!`, `cfg_coop!`,
// `cfg_io_util!`, etc. — replacing the `cfg_NAME! {` prefix and the matching `}`
// with spaces, so the items inside parse in place with their real enclosing
// impl/module and unchanged line numbers (tree-sitter otherwise leaves a macro
// body an opaque token_tree, hiding every fn/impl/struct within: 317 sites in
// tokio/src). Byte offsets are preserved, so downstream extraction is unchanged.
// Returns the rewritten source, or empty if nothing matched.
[[nodiscard]] std::string rust_blank_cfg_macros(std::string_view src) {
  std::string out(src);
  const auto n = out.size();
  bool changed = false;
  const auto blank = [&](std::size_t a, std::size_t b) {
    for (std::size_t k = a; k < b; ++k) {
      if (out[k] != '\n') out[k] = ' ';
    }
  };
  std::size_t i = 0;
  while (i < n) {
    if (out[i] == '/' || out[i] == '"' || out[i] == '\'') {
      const auto next = rust_skip_inert(out, i);
      i = next > i ? next : i + 1;
      continue;
    }
    // Match `cfg_<word>! <ws>* {` at an identifier boundary.
    const bool boundary = i == 0 || (!std::isalnum(static_cast<unsigned char>(out[i - 1])) && out[i - 1] != '_');
    if (boundary && out.compare(i, 4, "cfg_") == 0) {
      std::size_t k = i + 4;
      while (k < n && (std::isalnum(static_cast<unsigned char>(out[k])) || out[k] == '_')) ++k;
      if (k < n && out[k] == '!') {
        std::size_t m = k + 1;
        while (m < n && std::isspace(static_cast<unsigned char>(out[m]))) ++m;
        if (m < n && out[m] == '{') {
          std::size_t depth = 1;
          std::size_t p = m + 1;
          while (p < n && depth > 0) {
            if (out[p] == '/' || out[p] == '"' || out[p] == '\'') {
              const auto next = rust_skip_inert(out, p);
              p = next > p ? next : p + 1;
              continue;
            }
            if (out[p] == '{') ++depth;
            else if (out[p] == '}') --depth;
            ++p;
          }
          if (depth == 0) {
            blank(i, m + 1);   // `cfg_NAME! {`
            blank(p - 1, p);   // the matching `}`
            changed = true;
            i = m + 1;         // keep scanning inside the now-unwrapped body
            continue;
          }
        }
      }
    }
    ++i;
  }
  return changed ? out : std::string();
}

[[nodiscard]] LanguageConfig rust_config() {
  LanguageConfig config{
      .name = "rust",
      .grammar_name = "tree-sitter-rust",
      .extensions = {".rs"},
      // No class kind in Rust: struct/enum/union/trait/type-alias are all "type"
      // nodes (mirrors Go's type_spec choice). impl blocks carry no name and are
      // deliberately not registered -- methods inside them are function_items,
      // captured as file-contained functions and tagged methods by
      // rust_is_impl_method (the node type alone cannot tell them from free
      // functions, so method_node_types does not apply).
      .function_node_types = {"function_item", "function_signature_item"},
      .type_node_types = {"struct_item", "enum_item", "union_item", "trait_item", "type_item"},
      .import_node_types = {"use_declaration", "mod_item"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
      // `x.method()` is call_expression{function: field_expression}; the bare
      // method name is the `field`. Qualified `Type::method()` is a
      // scoped_identifier (not a member call) and is reduced by rust_callee_name.
      .call_member_node_types = {"field_expression"},
      .call_member_field = "field",
      .resolve_callee_name = rust_callee_name,
  };
  config.import_handler = rust_import_handler;
  config.relation_handler = rust_relation_handler;
  config.extract_members = true;
  config.member_handler = rust_member_handler;
  config.extra_walk = rust_extra_walk;
  config.method_predicate = [](const TSNode& node, const ExtractionContext&) {
    return rust_is_impl_method(node);
  };
  config.preprocess_source = rust_blank_cfg_macros;
  return config;
}

[[nodiscard]] LanguageConfig groovy_config() {
  return LanguageConfig{
      .name = "groovy",
      .grammar_name = "tree-sitter-groovy",
      .extensions = {".groovy", ".gvy", ".gradle"},
      .class_node_types = {"class_definition"},
      .function_node_types = {"function_declaration", "function_definition"},
      .import_node_types = {"groovy_import"},
      .call_node_types = {"function_call", "juxt_function_call"},
      .name_fields = {"name", "function"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
  };
}

}  // namespace

const TSLanguage* tree_sitter_language_for(DetectedLanguage language) {
  switch (language) {
    case DetectedLanguage::C:
      return tree_sitter_c();
    case DetectedLanguage::Cpp:
      return tree_sitter_cpp();
    case DetectedLanguage::CSharp:
      return tree_sitter_c_sharp();
    case DetectedLanguage::Go:
      return tree_sitter_go();
    case DetectedLanguage::Groovy:
      return tree_sitter_groovy();
    case DetectedLanguage::Java:
      return tree_sitter_java();
    case DetectedLanguage::JavaScript:
      return tree_sitter_javascript();
    case DetectedLanguage::Kotlin:
      return tree_sitter_kotlin();
    case DetectedLanguage::Python:
      return tree_sitter_python();
    case DetectedLanguage::Ruby:
      return tree_sitter_ruby();
    case DetectedLanguage::Rust:
      return tree_sitter_rust();
    case DetectedLanguage::Scala:
      return tree_sitter_scala();
    case DetectedLanguage::TypeScript:
      return tree_sitter_typescript();
    case DetectedLanguage::Tsx:
      return tree_sitter_tsx();
    default:
      return nullptr;
  }
}

std::optional<LanguageConfig> config_for_language(DetectedLanguage language) {
  switch (language) {
    case DetectedLanguage::C:
      return c_config();
    case DetectedLanguage::Cpp:
      return cpp_config();
    case DetectedLanguage::CSharp:
      return csharp_config();
    case DetectedLanguage::Go:
      return go_config();
    case DetectedLanguage::Groovy:
      return groovy_config();
    case DetectedLanguage::Java:
      return java_config();
    case DetectedLanguage::JavaScript:
      return javascript_language_config();
    case DetectedLanguage::Kotlin:
      return kotlin_config();
    case DetectedLanguage::Python:
      return python_language_config();
    case DetectedLanguage::Ruby:
      return ruby_config();
    case DetectedLanguage::Rust:
      return rust_config();
    case DetectedLanguage::Scala:
      return scala_config();
    case DetectedLanguage::TypeScript:
      return typescript_language_config();
    case DetectedLanguage::Tsx:
      return tsx_language_config();
    default:
      return std::nullopt;
  }
}

namespace {

[[nodiscard]] std::optional<ExtractionResult> extract_language(DetectedLanguage language, const ExtractionContext& context) {
  if (auto result = extract_non_grammar_language(language, context); result.has_value()) {
    return result;
  }

  const auto* grammar = tree_sitter_language_for(language);
  auto config = config_for_language(language);
  if (grammar == nullptr || !config.has_value()) {
    return std::nullopt;
  }

  intern_node_symbols(*config, grammar);
  if (language == DetectedLanguage::Python) {
    return extract_python(context);
  }
  if (language == DetectedLanguage::JavaScript) {
    return extract_javascript(context);
  }
  if (language == DetectedLanguage::TypeScript) {
    return extract_typescript(context);
  }
  if (language == DetectedLanguage::Tsx) {
    return extract_tsx(context);
  }
  return extract_with_config(grammar, *config, context);
}

}  // namespace

std::optional<ExtractionResult> extract_configured_language(
    DetectedLanguage language,
    const ExtractionContext& context) {
  auto result = extract_language(language, context);
  // A build file is also read for the Spring Boot Actuator it declares, beside
  // its own grammar's (or the XML) extraction.
  if (result.has_value() && is_spring_build_file(context.relative_path)) {
    append_spring_actuator_facts(context, *result);
  }
  return result;
}

bool has_registered_extractor(DetectedLanguage language) {
  if (handles_non_grammar_language(language)) {
    return true;
  }
  return tree_sitter_language_for(language) != nullptr && config_for_language(language).has_value();
}

std::map<std::string, std::size_t> unextracted_counts(std::span<const DetectedFile> files) {
  std::map<std::string, std::size_t> counts;
  for (const auto& file : files) {
    if (file.language == DetectedLanguage::Unknown || has_registered_extractor(file.language)) {
      continue;
    }
    ++counts[std::string(language_name(file.language))];
  }
  return counts;
}

}  // namespace cgraph
