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

  if (type == "type_identifier" || type == "qualified_identifier" || type == "namespace_identifier") {
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

}  // namespace

std::string cpp_callee_name(const TSNode& node, const ExtractionContext& context) {
  return callee_leaf_name(node, context.source, 0);
}

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
      .source = make_id(context.source_file),
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

void cpp_field_walk(const TSNode& node, const ExtractionContext& context, Fragment& fragment, std::vector<RawCall>&) {
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
  const auto class_id = make_id(context.source_file + ":" + class_name);

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
    const auto field_id = make_id(context.source_file + ":" + class_name + "::" + field_name);
    fragment.nodes.push_back(Node{
        .id = field_id,
        .label = field_name,
        .source_file = context.source_file,
        .source_location = source_location(member),
        .kind = "field",
        .confidence = Confidence::Extracted,
    });
    fragment.edges.push_back(Edge{
        .source = class_id,
        .target = field_id,
        .relation = "defines",
        .confidence = Confidence::Extracted,
    });
  }
}

}  // namespace cgraph
