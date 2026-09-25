#include "cgraph/cpp_extractor.hpp"

#include "cgraph/normalize.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

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

// Reduce a possibly-qualified name to its tail: `std::vector` -> `vector`.
[[nodiscard]] std::string name_tail(std::string text) {
  if (const auto pos = text.rfind("::"); pos != std::string::npos) {
    return text.substr(pos + 2);
  }
  return text;
}

[[nodiscard]] bool is_cpp_primitive(std::string_view name) {
  static const std::unordered_set<std::string_view> primitives = {
      "void", "bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t",
      "short", "int", "long", "float", "double", "signed", "unsigned",
      "auto", "size_t", "nullptr_t", "true", "false",
  };
  return primitives.contains(name);
}

// Walk a C/C++ type subtree, appending (name, is_generic_arg) for each referenced
// user type, skipping primitives. Mirrors the JS collect_type_refs: a
// `template_type` yields its base name and recurses into its `<...>` arguments;
// qualified and plain type identifiers are leaves reduced to their tail.
void collect_type_refs(const TSNode& node, std::string_view source, bool generic, std::vector<std::pair<std::string, bool>>& out) {
  if (ts_node_is_null(node)) {
    return;
  }
  const std::string_view type = ts_node_type(node);
  const auto emit = [&](std::string text) {
    text = name_tail(std::move(text));
    if (!text.empty() && !is_cpp_primitive(text)) {
      out.emplace_back(std::move(text), generic);
    }
  };

  if (type == "qualified_identifier") {
    // `std::vector<Node>` parses as qualified_identifier(scope: std, name:
    // template_type(vector, <Node>)): the template and its arguments live in
    // the `name` child. Treating the qualified text as a leaf emitted the tail
    // "vector<Node>" and never walked the arguments, so no namespace-qualified
    // template ever produced a generic_arg reference -- every signature naming
    // RawCall in this engine is `std::span<const RawCall>` or
    // `std::vector<RawCall>&`, and RawCall had no incoming reference (#94).
    const auto name = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name)) {
      const std::string_view name_type = ts_node_type(name);
      if (name_type == "template_type" || name_type == "qualified_identifier") {
        collect_type_refs(name, source, generic, out);
        return;
      }
    }
    emit(node_text(node, source));
    return;
  }
  if (type == "type_identifier" || type == "namespace_identifier") {
    emit(node_text(node, source));
    return;
  }
  if (type == "primitive_type" || type == "sized_type_specifier" || type == "placeholder_type_specifier") {
    return;
  }
  if (type == "template_type") {
    if (const auto name = ts_node_child_by_field_name(node, "name", 4); !ts_node_is_null(name)) {
      emit(node_text(name, source));
    }
    const auto count = ts_node_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto child = ts_node_child(node, index);
      if (std::string_view(ts_node_type(child)) != "template_argument_list") {
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
  // Wrappers (pointer/reference/const-qualified/type_descriptor): recurse named children.
  const auto count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto child = ts_node_child(node, index);
    if (ts_node_is_named(child)) {
      collect_type_refs(child, source, generic, out);
    }
  }
}

// Descend a declarator (through pointer/reference/array/parenthesized wrappers)
// to the leaf identifier that names the field, function, or variable.
[[nodiscard]] std::string declarator_name(const TSNode& node, std::string_view source) {
  if (ts_node_is_null(node)) {
    return {};
  }
  const std::string_view type = ts_node_type(node);
  if (type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
      type == "qualified_identifier" || type == "destructor_name" || type == "operator_name") {
    return name_tail(node_text(node, source));
  }
  if (const auto inner = ts_node_child_by_field_name(node, "declarator", 10); !ts_node_is_null(inner)) {
    return declarator_name(inner, source);
  }
  // Not every declarator wrapper exposes its inner declarator as a `declarator`
  // field -- tree-sitter-cpp's reference_declarator is `seq('&', _declarator)`
  // with an unnamed child -- so a field lookup alone leaves a reference-returning
  // function unnamed (the label stayed `& lock_map_mutex()`). Scan named children
  // for the first that yields a name. Gated on the `_declarator` suffix so this
  // never wanders out of the declarator subtree and into a function body.
  if (type.ends_with("_declarator")) {
    const std::uint32_t count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      if (auto name = declarator_name(ts_node_named_child(node, index), source); !name.empty()) {
        return name;
      }
    }
  }
  return {};
}

[[nodiscard]] bool is_function_declarator(const TSNode& node) {
  if (ts_node_is_null(node)) {
    return false;
  }
  const std::string_view type = ts_node_type(node);
  if (type == "function_declarator") {
    return true;
  }
  // A pointer/reference to a function still declares a function member.
  if (const auto inner = ts_node_child_by_field_name(node, "declarator", 10); !ts_node_is_null(inner)) {
    return is_function_declarator(inner);
  }
  return false;
}

[[nodiscard]] std::string class_name_of(const TSNode& node, std::string_view source) {
  const auto name = ts_node_child_by_field_name(node, "name", 4);
  return ts_node_is_null(name) ? std::string{} : node_text(name, source);
}

}  // namespace

std::string cpp_function_name(const TSNode& node, const ExtractionContext& context) {
  return declarator_name(node, context.source);
}

namespace {

// The leaf name of a call's callee, reached through the grammar rather than by
// string surgery.
//
// A previous version reduced the callee's TEXT at its last `::`. That is unsafe
// in both directions on real C++: `ns::make<zoo::Beast>` reduces to `Beast>`
// (make_id then drops the `>`), fabricating a call to an unrelated struct and
// losing the call to `make`; and `Outer<zoo::Beast>::make` split at the FIRST
// `::` yields the nonsense scope `Outer<zoo`. `::` legitimately appears in nine
// distinct callee node types, so no text rule can tell them apart.
//
// Shapes handled, all verified against the vendored grammar:
//   qualified_identifier -> descend `name` (right-nested, arbitrary depth)
//   template_function    -> its `name` identifier      (`make<T>`  -> `make`)
//   template_method      -> its `name` field_identifier (`tmpl<T>` -> `tmpl`)
//   dependent_name       -> skip the bare `template` token, descend the child
//   terminals            -> identifier / field_identifier / type_identifier /
//                           operator_name / destructor_name
[[nodiscard]] std::string callee_leaf_name(const TSNode& node, std::string_view source, int depth) {
  if (ts_node_is_null(node) || depth > 24) {
    return {};
  }
  const std::string_view type = ts_node_type(node);
  if (type == "identifier" || type == "field_identifier" || type == "type_identifier" ||
      type == "operator_name" || type == "destructor_name") {
    return std::string(node_text(node, source));
  }
  if (type == "qualified_identifier") {
    // No `scope` child means explicit GLOBAL scope (`::stat(...)`): a platform
    // symbol, not a project one. Refuse it, so it cannot bind to a same-named
    // local and report a false dependent.
    if (ts_node_is_null(ts_node_child_by_field_name(node, "scope", 5))) {
      return {};
    }
    return callee_leaf_name(ts_node_child_by_field_name(node, "name", 4), source, depth + 1);
  }
  if (type == "template_function" || type == "template_method") {
    return callee_leaf_name(ts_node_child_by_field_name(node, "name", 4), source, depth + 1);
  }
  if (type == "dependent_name") {
    // `template f<X>` -- the `template` keyword is a bare anonymous child, so walk
    // the named children instead of looking for a field.
    const std::uint32_t count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      if (auto name = callee_leaf_name(ts_node_named_child(node, index), source, depth + 1); !name.empty()) {
        return name;
      }
    }
  }
  return {};
}

// The `::`-joined scopes a qualified name spells, outermost first. `a::b::f`
// parses as qualified_identifier(scope: a, name: qualified_identifier(scope: b,
// name: f)), so collect each scope while descending to the leaf. A scope is
// recorded as its bare identifier only -- `Outer<proj::Beast>` is recorded as
// `Outer` -- so every segment is `::`-free and the resolver can split the joined
// text on `::` without ever cutting inside a template argument (the
// fabricated-name failure callee_leaf_name exists to prevent).
//
// Empty for an unqualified name, and empty when a scope names nothing a
// declaration could carry (`decltype(x)::f`): the resolver's scope gate runs
// only on a non-empty qualifier, so such a call resolves on its bare name.
[[nodiscard]] std::string qualified_scope(const TSNode& node, std::string_view source) {
  std::string scope;
  TSNode current = node;
  for (int depth = 0; depth < 24 && !ts_node_is_null(current); ++depth) {
    const std::string_view type = ts_node_type(current);
    if (type == "template_function" || type == "template_method") {
      current = ts_node_child_by_field_name(current, "name", 4);
      continue;
    }
    if (type != "qualified_identifier") {
      break;
    }
    const auto scope_node = ts_node_child_by_field_name(current, "scope", 5);
    if (!ts_node_is_null(scope_node)) {
      std::string segment;
      const std::string_view scope_type = ts_node_type(scope_node);
      if (scope_type == "template_type") {
        if (const auto name = ts_node_child_by_field_name(scope_node, "name", 4); !ts_node_is_null(name)) {
          segment = std::string(node_text(name, source));
        }
      } else if (scope_type == "namespace_identifier" || scope_type == "type_identifier" ||
                 scope_type == "identifier") {
        segment = std::string(node_text(scope_node, source));
      }
      if (segment.empty() || segment.find("::") != std::string::npos || segment.find('<') != std::string::npos) {
        return {};
      }
      if (!scope.empty()) {
        scope += "::";
      }
      scope += segment;
    }
    current = ts_node_child_by_field_name(current, "name", 4);
  }
  return scope;
}

// The outermost segment of a `::`-joined scope: the only one a template
// parameter or a namespace alias can be.
[[nodiscard]] std::string scope_root(const std::string& scope) {
  return scope.substr(0, scope.find("::"));
}

// Whether `name` is a type parameter of a template enclosing `node`. `T::make()`
// inside `template <typename T>` names the scope of whatever T is instantiated
// with, which no declaration in the project carries.
[[nodiscard]] bool names_template_parameter(const TSNode& node, const std::string& name, std::string_view source) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (std::string_view(ts_node_type(parent)) != "template_declaration") {
      continue;
    }
    const auto parameters = ts_node_child_by_field_name(parent, "parameters", 10);
    if (ts_node_is_null(parameters)) {
      continue;
    }
    const std::uint32_t count = ts_node_named_child_count(parameters);
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto parameter = ts_node_named_child(parameters, index);
      const std::uint32_t parts = ts_node_named_child_count(parameter);
      for (std::uint32_t part = 0; part < parts; ++part) {
        const auto named = ts_node_named_child(parameter, part);
        if (std::string_view(ts_node_type(named)) == "type_identifier" && node_text(named, source) == name) {
          return true;
        }
      }
    }
  }
  return false;
}

// The namespace a `namespace <name> = proj::detail;` alias stands for, looked up
// from `node` outwards, or empty when no enclosing scope declares such an alias.
// The alias is a local spelling; the namespace is what the declaration carries,
// so resolution has to see through it.
[[nodiscard]] std::string namespace_alias_target(const TSNode& node, const std::string& name, std::string_view source) {
  // C++ binds an alias before it is used, so each enclosing scope is scanned
  // only up to the call itself, with a cursor: `ts_node_named_child(parent, i)`
  // descends from the parent every time, which turns one file-level scan into
  // quadratic work (measured: +70 ms of extraction on this repo's own src/).
  const auto before = ts_node_start_byte(node);
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    std::string target;
    if (ts_tree_cursor_goto_first_child(&cursor)) {
      do {
        const auto child = ts_tree_cursor_current_node(&cursor);
        if (ts_node_start_byte(child) >= before) {
          break;
        }
        if (std::string_view(ts_node_type(child)) != "namespace_alias_definition") {
          continue;
        }
        const auto alias = ts_node_child_by_field_name(child, "name", 4);
        if (ts_node_is_null(alias) || node_text(alias, source) != name) {
          continue;
        }
        // The aliased namespace is the definition's other named child: a
        // `namespace_identifier` for `= detail`, a `nested_namespace_specifier`
        // for `= proj::detail`.
        const std::uint32_t parts = ts_node_named_child_count(child);
        for (std::uint32_t part = 0; part < parts; ++part) {
          const auto aliased = ts_node_named_child(child, part);
          const std::string_view aliased_type = ts_node_type(aliased);
          if (!ts_node_eq(aliased, alias) &&
              (aliased_type == "namespace_identifier" || aliased_type == "nested_namespace_specifier")) {
            target = node_text(aliased, source);
          }
        }
      } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    if (!target.empty()) {
      return target;
    }
  }
  return {};
}

// The scope a definition's own declarator names: `int proj::Cache::reload() {}`
// declares a member of `proj::Cache`, and nothing else records that owner --
// the in-class prototype is a field_declaration, which gets no node of its own,
// and declarator_name reduces the definition's label to `reload`.
[[nodiscard]] std::string declarator_scope(const TSNode& node, std::string_view source) {
  if (ts_node_is_null(node)) {
    return {};
  }
  if (std::string_view(ts_node_type(node)) == "qualified_identifier") {
    return qualified_scope(node, source);
  }
  return declarator_scope(ts_node_child_by_field_name(node, "declarator", 10), source);
}

}  // namespace

std::string cpp_callee_name(const TSNode& node, const ExtractionContext& context) {
  return callee_leaf_name(node, context.source, 0);
}

std::string cpp_callee_scope(const TSNode& node, const ExtractionContext& context) {
  auto scope = qualified_scope(node, context.source);
  if (scope.empty()) {
    return {};
  }
  const auto root = scope_root(scope);
  // A dependent scope names no declaration, so the whole qualifier is refused
  // and the call resolves on its bare name; an aliased one names a real
  // namespace under a local spelling, so it is rewritten to that namespace.
  if (names_template_parameter(node, root, context.source)) {
    return {};
  }
  if (const auto aliased = namespace_alias_target(node, root, context.source); !aliased.empty()) {
    scope.replace(0, root.size(), aliased);
  }
  return scope;
}

namespace {

// The `::`-joined names of the namespaces enclosing `node`, outermost first, or
// empty at file scope. An anonymous namespace contributes "(anonymous)".
[[nodiscard]] std::string enclosing_namespace_scope(const TSNode& node, std::string_view source) {
  std::vector<std::string> names;
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent); parent = ts_node_parent(parent)) {
    if (std::string_view(ts_node_type(parent)) != "namespace_definition") {
      continue;
    }
    // An inline namespace is transparent to qualified lookup -- `proj::f()`
    // finds a declaration in `proj::v1` when v1 is inline -- so it contributes
    // no segment, and a call that spells it is the one that no longer matches.
    if (const auto first = ts_node_child(parent, 0);
        !ts_node_is_null(first) && node_text(first, source) == "inline") {
      continue;
    }
    const auto name = ts_node_child_by_field_name(parent, "name", 4);
    names.push_back(ts_node_is_null(name) ? std::string("(anonymous)") : std::string(node_text(name, source)));
  }
  std::string scope;
  for (auto it = names.rbegin(); it != names.rend(); ++it) {
    if (!scope.empty()) {
      scope += "::";
    }
    scope += *it;
  }
  return scope;
}

// Stamps `scope` on the symbol node the walk just added for `node`, so call
// resolution can check a qualified callee against the declaration's own scope:
// the enclosing namespaces, plus whatever the declarator itself qualifies
// (`int proj::Cache::reload() {}` is declared in `proj::Cache`). Nothing is
// stamped at file scope, keeping namespace-free graphs byte-identical.
void stamp_namespace_scope(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  const std::string_view node_type = ts_node_type(node);
  if (node_type != "function_definition" && node_type != "class_specifier" && node_type != "struct_specifier" &&
      node_type != "union_specifier" && node_type != "enum_specifier") {
    return;
  }
  auto scope = enclosing_namespace_scope(node, context.source);
  if (const auto declared = declarator_scope(node, context.source); !declared.empty()) {
    if (!scope.empty()) {
      scope += "::";
    }
    scope += declared;
  }
  if (scope.empty()) {
    return;
  }
  const auto location = source_location(node);
  for (auto it = fragment.nodes.rbegin(); it != fragment.nodes.rend(); ++it) {
    if (it->source_file != context.source_file || !it->source_location.has_value() ||
        it->kind == "field" || it->source_location->start_line != location.start_line ||
        it->source_location->start_column != location.start_column) {
      continue;
    }
    it->properties["scope"] = scope;
    return;
  }
}

}  // namespace

void cpp_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  if (std::string_view(ts_node_type(node)) != "preproc_include") {
    return;
  }
  const auto path = ts_node_child_by_field_name(node, "path", 4);
  if (ts_node_is_null(path)) {
    return;
  }
  auto spec = node_text(path, context.source);
  if (spec.size() >= 2) {
    spec = spec.substr(1, spec.size() - 2);  // strip the surrounding "" or <>
  }
  if (spec.empty()) {
    return;
  }

  // Carry the include spec verbatim (`cgraph/types.hpp`, `base.hpp`, `vector`).
  // resolve_imports matches it against project files by path suffix — the way an
  // include directory actually resolves a header — so `#include "cgraph/x.hpp"`
  // finds src/engine/include/cgraph/x.hpp without us knowing the include roots.
  // A spec that matches no project file (a system/third-party header) is dropped,
  // leaving no dangling edge.
  const auto import_path = spec;
  const auto module_id = make_id(import_path);
  fragment.nodes.push_back(Node{
      .id = module_id,
      .label = spec,
      .source_location = SourceLocation{.start_line = 1, .end_line = 1},
      .kind = "module",
      .confidence = Confidence::Extracted,
      .properties = {{"import_path", import_path}},
  });
  fragment.edges.push_back(Edge{
      .source = make_id(context.relative_path),
      .target = module_id,
      .relation = "imports",
      .confidence = Confidence::Extracted,
  });
}

namespace {

// Emit a `references` relation for every user type named in a type subtree.
void emit_type_refs(const TSNode& type_node, const ExtractionContext& context, const std::string& source_id,
                    const char* ref_context, std::vector<RawRelation>& out) {
  std::vector<std::pair<std::string, bool>> refs;
  collect_type_refs(type_node, context.source, false, refs);
  for (auto& [name, is_generic] : refs) {
    out.push_back(RawRelation{
        .source_id = source_id,
        .target_label = std::move(name),
        .relation = "references",
        .context = is_generic ? "generic_arg" : ref_context,
        .source_file = context.source_file,
        .allow_same_file = false,  // references resolve only through includes
    });
  }
}

// References from a function's return type and parameter types, attributed to
// the function node. Covers free functions and (via the generic walk) methods.
void emit_function_refs(const TSNode& node, const ExtractionContext& context, const std::string& node_id,
                        std::vector<RawRelation>& out) {
  if (const auto return_type = ts_node_child_by_field_name(node, "type", 4); !ts_node_is_null(return_type)) {
    emit_type_refs(return_type, context, node_id, "return_type", out);
  }
  const auto declarator = ts_node_child_by_field_name(node, "declarator", 10);
  if (!is_function_declarator(declarator)) {
    return;
  }
  const auto params = ts_node_child_by_field_name(declarator, "parameters", 10);
  if (ts_node_is_null(params)) {
    return;
  }
  const auto param_count = ts_node_child_count(params);
  for (std::uint32_t param = 0; param < param_count; ++param) {
    const auto parameter = ts_node_child(params, param);
    if (std::string_view(ts_node_type(parameter)) != "parameter_declaration") {
      continue;
    }
    if (const auto param_type = ts_node_child_by_field_name(parameter, "type", 4); !ts_node_is_null(param_type)) {
      emit_type_refs(param_type, context, node_id, "parameter_type", out);
    }
  }
}

}  // namespace

void cpp_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id, std::vector<RawRelation>& out) {
  const std::string_view node_type = ts_node_type(node);

  // Functions (free or method) reached via the generic walk: references from
  // their signature types.
  if (node_type == "function_definition") {
    emit_function_refs(node, context, node_id, out);
    return;
  }

  if (node_type != "class_specifier" && node_type != "struct_specifier") {
    return;
  }

  // Base classes -> inherits.
  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    const auto child = ts_node_child(node, index);
    if (std::string_view(ts_node_type(child)) != "base_class_clause") {
      continue;
    }
    const auto base_count = ts_node_child_count(child);
    for (std::uint32_t base = 0; base < base_count; ++base) {
      const auto base_node = ts_node_child(child, base);
      const std::string_view base_type = ts_node_type(base_node);
      if (base_type != "type_identifier" && base_type != "qualified_identifier" && base_type != "template_type") {
        continue;
      }
      std::vector<std::pair<std::string, bool>> names;
      collect_type_refs(base_node, context.source, false, names);
      for (auto& [name, is_generic] : names) {
        (void)is_generic;
        out.push_back(RawRelation{
            .source_id = node_id,
            .target_label = std::move(name),
            .relation = "inherits",
            .context = "type",
            .source_file = context.source_file,
            .allow_same_file = true,  // a base class may be declared in the same file
        });
      }
    }
  }

  // Data members -> references from the owning type. (Method signature refs are
  // emitted via the function branch above as the walk descends into the body.)
  const auto body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(body)) {
    return;
  }
  const auto member_count = ts_node_child_count(body);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    const auto member = ts_node_child(body, index);
    if (std::string_view(ts_node_type(member)) != "field_declaration") {
      continue;
    }
    const auto declarator = ts_node_child_by_field_name(member, "declarator", 10);
    if (is_function_declarator(declarator)) {
      continue;  // a method declaration, not a data member
    }
    if (const auto type_node = ts_node_child_by_field_name(member, "type", 4); !ts_node_is_null(type_node)) {
      emit_type_refs(type_node, context, node_id, "field", out);
    }
  }
}

void cpp_field_walk(const TSNode& node, const ExtractionContext& context, const std::string& /*function_scope_id*/, Fragment& fragment, std::vector<RawCall>&, std::vector<RawRelation>&) {
  stamp_namespace_scope(node, context, fragment);
  const std::string_view node_type = ts_node_type(node);
  if (node_type != "class_specifier" && node_type != "struct_specifier") {
    return;
  }
  const auto class_name = class_name_of(node, context.source);
  if (class_name.empty()) {
    return;  // anonymous struct/union: no stable owner to define members on
  }
  const auto body = ts_node_child_by_field_name(node, "body", 4);
  if (ts_node_is_null(body)) {
    return;
  }
  const auto class_id = make_id(context.relative_path + ":" + class_name);

  const auto member_count = ts_node_child_count(body);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    const auto member = ts_node_child(body, index);
    if (std::string_view(ts_node_type(member)) != "field_declaration") {
      continue;
    }
    const auto declarator = ts_node_child_by_field_name(member, "declarator", 10);
    if (is_function_declarator(declarator)) {
      continue;  // a method declaration, not a data member (handled by the walk)
    }
    const auto field_name = declarator_name(declarator, context.source);
    if (field_name.empty()) {
      continue;
    }
    add_field_node(context, class_id, class_name, field_name, source_location(member), {}, fragment);
  }
}

}  // namespace cgraph
