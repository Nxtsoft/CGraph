#include "cgraph/extractor.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

extern "C" const TSLanguage* tree_sitter_c();
extern "C" const TSLanguage* tree_sitter_cpp();

namespace {

[[nodiscard]] cgraph::LanguageConfig cpp_class_config() {
  cgraph::LanguageConfig config{
      .name = "cpp",
      .grammar_name = "tree-sitter-cpp",
      .extensions = {".cpp"},
      .class_node_types = {"class_specifier", "struct_specifier", "union_specifier", "enum_specifier"},
      .function_node_types = {"function_definition"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name", "declarator"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
  };
  config.class_requires_body = true;
  cgraph::intern_node_symbols(config, tree_sitter_cpp());
  return config;
}

[[nodiscard]] std::vector<const cgraph::Node*> class_nodes(
    const cgraph::Fragment& fragment, std::string_view label) {
  std::vector<const cgraph::Node*> found;
  for (const auto& node : fragment.nodes) {
    if (node.kind == "class" && node.label == label) {
      found.push_back(&node);
    }
  }
  return found;
}

// `class_requires_body` (extractor.cpp): a class-like specifier with no body is
// a declaration, not a definition, and must not mint a node -- otherwise a
// forward declaration and its definition become two nodes for one type, and
// every use that resolves to the declaration points at a line with no members.
[[nodiscard]] bool check_forward_declarations() {
  const auto config = cpp_class_config();
  struct Case {
    std::string_view name;
    std::string_view source;
    std::string_view label;
    std::size_t expected;
    std::uint32_t definition_line;
  };
  const Case cases[] = {
      {"declaration then definition",
       "class Later;\nclass Later { int x; };\n", "Later", 1, 2},
      {"friend declaration mints nothing",
       "class Host { friend class Friendly; };\n", "Friendly", 0, 0},
      {"opaque enum mints nothing",
       "enum class E : int;\n", "E", 0, 0},
      {"a variable of an elaborated type mints nothing",
       "struct stat st;\n", "stat", 0, 0},
      // A specialization is labelled by its full `name` field, so it is a
      // distinct label from the primary template's.
      {"explicit specialization declaration mints nothing",
       "template <typename T> struct S {};\ntemplate <> struct S<int>;\n", "S<int>", 0, 0},
      {"the primary template it declares against still mints",
       "template <typename T> struct S {};\ntemplate <> struct S<int>;\n", "S", 1, 1},
      {"body-bearing specialization still mints",
       "template <typename T> struct S {};\ntemplate <> struct S<int> { int y; };\n", "S<int>", 1, 2},
  };
  for (const auto& test : cases) {
    const auto result = cgraph::extract_with_config(
        tree_sitter_cpp(), config,
        cgraph::ExtractionContext{.source_file = "decl.cpp", .relative_path = "decl.cpp", .source = std::string(test.source)});
    const auto found = class_nodes(result.fragment, test.label);
    if (found.size() != test.expected) {
      std::cerr << test.name << ": expected " << test.expected << " `" << test.label
                << "` class nodes, saw " << found.size() << '\n';
      return false;
    }
    if (test.expected == 1 && found.front()->source_location->start_line != test.definition_line) {
      std::cerr << test.name << ": node sits at line " << found.front()->source_location->start_line
                << ", not the definition's line " << test.definition_line << '\n';
      return false;
    }
  }
  return true;
}

// Member extraction is opt-in: a language that sets no member_handler must
// produce the same fragment it produced before the hook existed, node for node
// and edge for edge.
[[nodiscard]] bool check_member_extraction_is_opt_in() {
  constexpr auto source = R"c(
struct Pair { int first; int second; };
int use(void) { return 0; }
)c";
  const auto context = cgraph::ExtractionContext{.source_file = "pair.c", .relative_path = "pair.c", .source = source};

  auto without = cgraph::LanguageConfig{
      .name = "c",
      .grammar_name = "tree-sitter-c",
      .extensions = {".c"},
      .class_node_types = {"struct_specifier"},
      .function_node_types = {"function_definition"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name", "declarator"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
  };
  cgraph::intern_node_symbols(without, tree_sitter_c());

  auto with = without;
  with.extract_members = true;
  with.member_handler = [](const TSNode& node, const cgraph::ExtractionContext& ctx,
                           const std::string& owner_id, cgraph::Fragment& fragment) {
    const auto body = ts_node_child_by_field_name(node, "body", 4);
    if (ts_node_is_null(body)) {
      return;
    }
    cgraph::add_field_node(ctx, owner_id, "Pair", "first",
                           cgraph::SourceLocation{.start_line = 2}, {}, fragment);
  };

  // The handler is only reached when the flag is set, so a config carrying a
  // handler with extract_members off must still emit nothing.
  auto handler_but_disabled = with;
  handler_but_disabled.extract_members = false;

  const auto baseline = cgraph::extract_with_config(tree_sitter_c(), without, context);
  const auto disabled = cgraph::extract_with_config(tree_sitter_c(), handler_but_disabled, context);
  const auto enabled = cgraph::extract_with_config(tree_sitter_c(), with, context);

  const auto has_fields = [](const cgraph::Fragment& fragment) {
    return std::ranges::any_of(fragment.nodes, [](const cgraph::Node& n) { return n.kind == "field"; }) ||
           std::ranges::any_of(fragment.edges, [](const cgraph::Edge& e) { return e.relation == "defines"; });
  };
  if (has_fields(baseline.fragment) || has_fields(disabled.fragment)) {
    std::cerr << "opt-in parity: a disabled language emitted field nodes or defines edges\n";
    return false;
  }
  if (baseline.fragment.nodes.size() != disabled.fragment.nodes.size() ||
      baseline.fragment.edges.size() != disabled.fragment.edges.size()) {
    std::cerr << "opt-in parity: fragment size changed with the handler present but disabled\n";
    return false;
  }
  for (std::size_t i = 0; i < baseline.fragment.nodes.size(); ++i) {
    const auto& a = baseline.fragment.nodes[i];
    const auto& b = disabled.fragment.nodes[i];
    if (a.id != b.id || a.label != b.label || a.kind != b.kind || a.properties != b.properties) {
      std::cerr << "opt-in parity: node " << i << " (" << a.label << ") differs\n";
      return false;
    }
  }
  for (std::size_t i = 0; i < baseline.fragment.edges.size(); ++i) {
    const auto& a = baseline.fragment.edges[i];
    const auto& b = disabled.fragment.edges[i];
    if (a.source != b.source || a.target != b.target || a.relation != b.relation) {
      std::cerr << "opt-in parity: edge " << i << " (" << a.relation << ") differs\n";
      return false;
    }
  }
  if (!has_fields(enabled.fragment)) {
    std::cerr << "opt-in parity: the enabled config emitted no field node, so the gate proves nothing\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!check_forward_declarations()) {
    return 1;
  }
  if (!check_member_extraction_is_opt_in()) {
    return 1;
  }

  auto config = cgraph::LanguageConfig{
      .name = "c",
      .grammar_name = "tree-sitter-c",
      .extensions = {".c", ".h"},
      .function_node_types = {"function_definition"},
      .call_node_types = {"call_expression"},
      .name_fields = {"declarator"},
      .call_accessor_fields = {"function"},
  };
  cgraph::intern_node_symbols(config, tree_sitter_c());

  constexpr auto source = R"c(
int helper(void) { return 0; }
int main(void) { return helper(); }
)c";

  const auto result = cgraph::extract_with_config(
      tree_sitter_c(),
      config,
      cgraph::ExtractionContext{.source_file = "main.c", .relative_path = "main.c", .source = source});

  // One file node plus the two functions it contains.
  if (result.fragment.nodes.size() != 3) {
    return 1;
  }
  std::size_t file_nodes = 0;
  for (const auto& node : result.fragment.nodes) {
    if (node.kind == "file") {
      ++file_nodes;
    }
  }
  if (file_nodes != 1) {
    return 1;
  }
  // The file should contain both functions via `contains` edges.
  std::size_t contains_edges = 0;
  for (const auto& edge : result.fragment.edges) {
    if (edge.relation == "contains") {
      ++contains_edges;
    }
  }
  if (contains_edges != 2) {
    return 1;
  }
  if (result.raw_calls.empty()) {
    return 1;
  }
  return 0;
}
