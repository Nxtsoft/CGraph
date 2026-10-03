#include "cgraph/extractor.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <iostream>
#include <set>
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

// add_symbol_node relocates a field that holds the id a symbol wants, and the
// extraction's id index (NodeIdIndexScope) must follow the rename: a later
// symbol wanting the field's NEW id has to find it there and move it again.
// An index left at the old id would let that symbol miss the field and take a
// line-suffixed id, leaving the function off the id an agent asks for.
[[nodiscard]] bool check_relocated_field_is_reindexed() {
  const auto result = cgraph::extract_python({.source_file = "c.py", .relative_path = "c.py", .source = R"py(
class First:
    size: int
def first_size():
    return 1
def first_size_size():
    return 2
)py"});
  const cgraph::Node* field = nullptr;
  std::set<std::string> ids;
  for (const auto& node : result.fragment.nodes) {
    if (!ids.insert(node.id).second) {
      std::cerr << "relocation: two nodes share id " << node.id << '\n';
      return false;
    }
    if (node.kind == "field" && node.label == "size") {
      field = &node;
    }
    if (node.kind == "function" && node.id != cgraph::make_id("c.py:" + node.label)) {
      std::cerr << "relocation: function " << node.label << " lost its natural id, got " << node.id << '\n';
      return false;
    }
  }
  // Moved once by `first_size` (to first_size:size) and again by
  // `first_size_size` (to first_size_size:size).
  if (field == nullptr || field->id != cgraph::make_id(cgraph::make_id("c.py:first_size_size") + ":size")) {
    std::cerr << "relocation: field id is " << (field == nullptr ? std::string("missing") : field->id) << '\n';
    return false;
  }
  const auto defines_field = std::ranges::any_of(result.fragment.edges, [&](const cgraph::Edge& edge) {
    return edge.relation == "defines" && edge.source == cgraph::make_id("c.py:First") && edge.target == field->id;
  });
  if (!defines_field) {
    std::cerr << "relocation: First's defines edge does not follow the field\n";
    return false;
  }
  return true;
}

// Each node costs a bounded number of make_id calls. add_symbol_node once
// normalized its seed again for every node already in the fragment, so a file
// with N symbols paid N^2 / 2 utf8proc normalizations (an 8,000-line file spent
// about 20 seconds there).
[[nodiscard]] bool check_make_id_calls_are_linear() {
  constexpr std::size_t kMembers = 400;
  std::string source = "class Big:\n";
  for (std::size_t i = 0; i < kMembers; ++i) {
    source += "    field" + std::to_string(i) + ": int\n";
  }
  for (std::size_t i = 0; i < kMembers; ++i) {
    source += "    def method" + std::to_string(i) + "(self):\n        return self.field" + std::to_string(i) + "\n";
  }
  const auto before = cgraph::make_id_calls();
  const auto result = cgraph::extract_python({.source_file = "big.py", .relative_path = "big.py", .source = source});
  const auto calls = cgraph::make_id_calls() - before;
  const auto nodes = result.fragment.nodes.size();
  if (nodes < 2 * kMembers || calls > 16 * nodes) {
    std::cerr << "make_id: " << calls << " calls for " << nodes << " nodes\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!check_relocated_field_is_reindexed()) {
    return 1;
  }
  if (!check_make_id_calls_are_linear()) {
    return 1;
  }
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
