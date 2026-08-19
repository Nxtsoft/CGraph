#include "cgraph/python_extractor.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

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

// Emits the same stub shape the JS handler does — a `module` stub with an
// `import_path` for resolve_imports to collapse onto the real file node, plus
// an `import` stub per imported name so `from x import Thing` can bind to the
// declared symbol. Stub ids are namespaced so they can never squat a real
// node's id (#42). The old handler emitted a dead-end node (whole statement as
// label, no import_path, no edges), which is why Python graphs carried no
// import relations at all (issue #45).
void python_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  const std::string_view statement_type = ts_node_type(node);
  const std::string file_id = make_id(context.source_file);

  const auto add_module_stub = [&](const std::string& resolved, const std::string& label) -> std::string {
    const auto module_id = make_id("import-module:" + resolved);
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
        add_module_stub(resolve_python_module_spec(context.source_file, spec), spec);
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
  const auto resolved = resolve_python_module_spec(context.source_file, spec);
  add_module_stub(resolved, spec);

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
    if (std::string_view(ts_node_type(child)) == "aliased_import") {
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
    const auto symbol_id = make_id("import-symbol:" + resolved + ":" + name);
    fragment.nodes.push_back(Node{
        .id = symbol_id,
        .label = name,
        .source_location = source_location(node),
        .kind = "import",
        .confidence = Confidence::Extracted,
        .properties = {{"import_path", resolved}},
    });
    fragment.edges.push_back(Edge{
        .source = file_id,
        .target = symbol_id,
        .relation = "imports",
        .confidence = Confidence::Extracted,
    });
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
  };
}

ExtractionResult extract_python(const ExtractionContext& context) {
  auto config = python_language_config();
  intern_node_symbols(config, tree_sitter_python());
  return extract_with_config(tree_sitter_python(), config, context);
}

}  // namespace cgraph
