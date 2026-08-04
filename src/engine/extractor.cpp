#include "cgraph/extractor.hpp"

#include "cgraph/normalize.hpp"
#include "cgraph/parser_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cgraph {
namespace {

struct TreeDeleter {
  void operator()(TSTree* tree) const noexcept {
    ts_tree_delete(tree);
  }
};

using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

[[nodiscard]] std::string node_text(const TSNode& node, std::string_view source) {
  const auto start = ts_node_start_byte(node);
  const auto end = ts_node_end_byte(node);
  if (start >= end || end > source.size()) {
    return {};
  }
  return std::string(source.substr(start, end - start));
}

[[nodiscard]] std::optional<TSNode> first_child_by_fields(const TSNode& node, const std::vector<std::string>& fields) {
  for (const auto& field : fields) {
    auto child = ts_node_child_by_field_name(node, field.data(), static_cast<std::uint32_t>(field.size()));
    if (!ts_node_is_null(child)) {
      return child;
    }
  }
  return std::nullopt;
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

[[nodiscard]] std::string label_for_node(
    const TSNode& node,
    const LanguageConfig& config,
    const ExtractionContext& context) {
  if (config.resolve_function_name) {
    if (auto resolved = config.resolve_function_name(node, context); !resolved.empty()) {
      return resolved;
    }
  }

  if (const auto child = first_child_by_fields(node, config.name_fields); child.has_value()) {
    return node_text(*child, context.source);
  }

  // No name could be resolved, so the construct is anonymous: an inline arrow or
  // callback in JS/TS (`onClick={() => ...}`, `arr.map(x => ...)`), or an
  // anonymous namespace / struct / union / enum in C/C++. Such a node must NOT
  // be labelled with its entire body — that produces a giant junk node that
  // floods the graph and dominates centrality rankings. Skip it; add_symbol_node
  // returns empty, the enclosing scope is unchanged, and the construct's members
  // are still walked and attached to the real scope that contains it. Named
  // constructs are recovered above via resolve_function_name or a name field.
  return {};
}

// An arrow function is a call scope (its body's calls are attributed to it)
// only when it is a module-level `const Foo = () => {}` declaration — the arrow
// is the value of a top-level variable_declarator whose lexical_declaration sits
// directly under the program (optionally behind an `export`). Every other arrow
// (a handler defined inside a component, an inline `.map(x => f(x))` callback,
// a JSX `onClick={() => ...}`) is a boundary: Graphify never seeds those bodies,
// so the calls inside them are dropped rather than attributed to the enclosing
// component. Matching this is what brings same-file call counts to parity.
[[nodiscard]] bool is_module_level_arrow(const TSNode& node) {
  if (const std::string_view type = ts_node_type(node); type != "arrow_function") {
    return false;
  }
  const TSNode declarator = ts_node_parent(node);
  if (ts_node_is_null(declarator) || std::string_view(ts_node_type(declarator)) != "variable_declarator") {
    return false;
  }
  const TSNode declaration = ts_node_parent(declarator);
  if (ts_node_is_null(declaration)) {
    return false;
  }
  if (const std::string_view type = ts_node_type(declaration);
      type != "lexical_declaration" && type != "variable_declaration") {
    return false;
  }
  TSNode ancestor = ts_node_parent(declaration);
  if (!ts_node_is_null(ancestor) && std::string_view(ts_node_type(ancestor)) == "export_statement") {
    ancestor = ts_node_parent(ancestor);
  }
  return !ts_node_is_null(ancestor) && std::string_view(ts_node_type(ancestor)) == "program";
}

// Returns the id of the node that was added, or an empty string when no symbol
// could be extracted. Callers use the returned id as the enclosing scope for
// nested calls.
std::string add_symbol_node(
    const TSNode& node,
    const LanguageConfig& config,
    const ExtractionContext& context,
    std::string_view kind,
    Fragment& fragment) {
  auto label = label_for_node(node, config, context);
  if (label.empty()) {
    return {};
  }

  const auto location = source_location(node);
  auto id = make_id(context.source_file + ":" + label);
  // A label names a symbol, so two symbols in one file can legitimately share
  // one: an overload set (`to_json` five times over), a constructor sharing its
  // class's name, or `operator=` for both copy and move. Their ids would collide
  // and merge_fragments would keep only the first, silently losing the rest --
  // and a lost symbol is worse than an awkward one, because an agent asking
  // where a function lives gets one of five answers with no hint the other four
  // exist. Disambiguate with the declaration's start line, which is stable for a
  // given file so the id stays deterministic. Only a colliding symbol pays; the
  // common case keeps the plain `file:label` id.
  const auto collides = [&](const std::string& candidate) {
    return std::ranges::any_of(fragment.nodes, [&](const Node& existing) { return existing.id == candidate; });
  };
  if (collides(id)) {
    // The line alone is not always enough: three overloads can share one line
    // (`int f(int); int f(double); int f(char);`), and a single retry would
    // recompute the same suffixed id for the third, which merge_fragments then
    // discards -- reintroducing exactly the silent loss this guard exists to
    // prevent. Add the column, then a counter, until the id is free.
    const auto base = context.source_file + ":" + label + ":" + std::to_string(location.start_line);
    id = make_id(base + ":" + std::to_string(location.start_column));
    for (std::size_t nth = 2; collides(id); ++nth) {
      id = make_id(base + ":" + std::to_string(location.start_column) + ":" + std::to_string(nth));
    }
  }
  fragment.nodes.push_back(Node{
      .id = id,
      .label = std::move(label),
      .source_file = context.source_file,
      .source_location = location,
      .kind = std::string(kind),
      .confidence = Confidence::Extracted,
  });
  return id;
}

void add_raw_call(
    const TSNode& node,
    const LanguageConfig& config,
    const ExtractionContext& context,
    const std::string& caller_id,
    std::vector<RawCall>& raw_calls) {
  std::string label;
  bool is_member_call = false;
  bool callee_resolver_ran = false;
  if (const auto child = first_child_by_fields(node, config.call_accessor_fields); child.has_value()) {
    // A member/property access target (`obj.method()`): record only the bare
    // property name and flag it, so resolution can keep it to the caller's own
    // file (the receiver type is unknown, so a project-wide name match would be
    // a guess). Other accessor children (a plain identifier callee) are taken
    // verbatim.
    const std::string_view child_type = ts_node_type(*child);
    const bool is_member = std::ranges::find(config.call_member_node_types, child_type) != config.call_member_node_types.end();
    if (is_member && !config.call_member_field.empty()) {
      const auto property = ts_node_child_by_field_name(
          *child, config.call_member_field.data(), static_cast<std::uint32_t>(config.call_member_field.size()));
      if (!ts_node_is_null(property)) {
        label = node_text(property, context.source);
        is_member_call = true;
      }
    }
    if (!is_member_call) {
      // A language-supplied resolver walks the grammar to the callee's leaf name;
      // it may also refuse the call (returning empty) for a callee that names
      // something outside the project, such as an explicitly global `::symbol`.
      if (config.resolve_callee_name) {
        label = config.resolve_callee_name(*child, context);
        callee_resolver_ran = true;
      } else {
        label = node_text(*child, context.source);
      }
    }
  } else {
    label = node_text(node, context.source);
  }

  if (label.empty()) {
    return;
  }

  // An empty label after the resolver ran is a deliberate refusal, not a miss.
  if (label.empty()) {
    return;
  }
  (void)callee_resolver_ran;

  raw_calls.push_back(RawCall{
      .caller_id = caller_id,
      .callee_label = std::move(label),
      .source_file = context.source_file,
      .source_location = source_location(node),
      .is_member_call = is_member_call,
  });
}

// Materializes the parent->child structural edge that Graphify emits for every
// nested declaration: a class owning a method becomes `method`, every other
// parent (file owning a symbol, function owning a nested helper) becomes
// `contains`. This is the structural backbone that connects otherwise-orphan
// symbol nodes into the same component as their file.
void add_containment_edge(
    const std::string& parent_id,
    std::string_view parent_kind,
    const std::string& child_id,
    std::string_view child_kind,
    Fragment& fragment) {
  if (parent_id.empty() || child_id.empty() || parent_id == child_id) {
    return;
  }
  const bool is_method = parent_kind == "class" && child_kind == "function";
  fragment.edges.push_back(Edge{
      .source = parent_id,
      .target = child_id,
      .relation = is_method ? "method" : "contains",
      .confidence = Confidence::Extracted,
  });
}

// Hard cap on AST recursion depth. walk_node descends one native stack frame per
// tree level. extract_files runs on std::thread workers, which get the default
// ~512 KB secondary-thread stack on macOS, so a pathologically deep parse blows
// that stack and the process dies with SIGBUS. Empirically the overflow lands
// between depth 250 and 500 on a 512 KB stack; the trigger seen in the wild was a
// machine-generated NumPy C header (__multiarray_api.h) with a single ~thousands-
// deep expression. Hand- and tool-written code nests well under 100 levels, so
// this bound sits comfortably above real code yet clear of the overflow. A file
// that reaches it records a warning instead of silently truncating, so a
// genuinely deep file surfaces rather than vanishing.
constexpr std::uint32_t kMaxWalkDepth = 200;
constexpr std::string_view kDepthCapMarker = "ast-depth-cap";

void walk_node(
    const TSNode& node,
    const LanguageConfig& config,
    const ExtractionContext& context,
    const std::string& scope_id,
    std::string_view scope_kind,
    const std::string& function_scope_id,
    Fragment& fragment,
    std::vector<RawCall>& raw_calls,
    std::vector<RawRelation>& raw_relations,
    std::uint32_t depth) {
  if (depth >= kMaxWalkDepth) {
    // Stop descending. Record once per file (warnings are few, scan is cheap) so
    // a genuinely deep file is visible and never silently dropped.
    const bool recorded = std::any_of(fragment.warnings.begin(), fragment.warnings.end(),
                                      [](const std::string& w) { return w.find(kDepthCapMarker) != std::string::npos; });
    if (!recorded) {
      fragment.warnings.push_back(std::string(kDepthCapMarker) + ": extraction stopped at depth " +
                                  std::to_string(kMaxWalkDepth) + " in " + context.source_file +
                                  " (deeper AST nodes skipped)");
    }
    return;
  }
  const auto symbol = ts_node_symbol(node);

  // Two scopes are threaded down the tree. `child_scope` is the structural
  // parent (innermost file/class/function/type) that owns `contains`/`method`
  // edges. `child_function_scope` is the innermost *function or method* only —
  // it is the caller for any call found beneath it. Graphify attributes calls
  // strictly to enclosing function bodies (never a file, class, or type scope),
  // so a call with no enclosing function is dropped rather than hung off a file
  // or type node.
  std::string child_scope = scope_id;
  std::string_view child_kind = scope_kind;
  std::string child_function_scope = function_scope_id;
  if (contains_symbol(config.symbols.class_nodes, symbol)) {
    if (auto id = add_symbol_node(node, config, context, "class", fragment); !id.empty()) {
      add_containment_edge(scope_id, scope_kind, id, "class", fragment);
      if (config.relation_handler) {
        config.relation_handler(node, context, id, raw_relations);
      }
      child_scope = std::move(id);
      child_kind = "class";
    }
  }
  if (contains_symbol(config.symbols.function_nodes, symbol)) {
    // Named function declarations and methods are always graph nodes and call
    // scopes (at any nesting). An arrow is one only when it is a module-level
    // `const Foo = () => {}`. Every other arrow — a handler defined inside a
    // component, an inline `.map(x => f(x))` callback, a JSX `onClick={() => …}`
    // — is a local: Graphify emits no node for it and seeds no call scope, so we
    // neither create a node nor attribute its calls (the body is a boundary).
    if (std::string_view(ts_node_type(node)) == "arrow_function" && !is_module_level_arrow(node)) {
      child_function_scope.clear();
    } else {
      auto id = add_symbol_node(node, config, context, "function", fragment);
      if (!id.empty()) {
        add_containment_edge(scope_id, scope_kind, id, "function", fragment);
        // Offer the function to the relation handler too, so a language can emit
        // references from its parameter/return types (C/C++ free functions and
        // methods). Handlers that only care about types/classes no-op here.
        if (config.relation_handler) {
          config.relation_handler(node, context, id, raw_relations);
        }
        child_scope = id;
        child_kind = "function";
      }
      child_function_scope = std::move(id);
    }
  }
  // Type-level declarations (TS interfaces, type aliases, enums). They carry a
  // `name` field and become first-class graph nodes like Graphify's, contained
  // by their file. They are not a call scope: a call lexically inside a type
  // alias keeps the enclosing function (if any) as its caller.
  if (contains_symbol(config.symbols.type_nodes, symbol)) {
    if (auto id = add_symbol_node(node, config, context, "type", fragment); !id.empty()) {
      add_containment_edge(scope_id, scope_kind, id, "type", fragment);
      if (config.relation_handler) {
        config.relation_handler(node, context, id, raw_relations);
      }
      child_scope = std::move(id);
      child_kind = "type";
    }
  }
  if (contains_symbol(config.symbols.import_nodes, symbol) && config.import_handler) {
    config.import_handler(node, context, fragment);
  }
  // Only calls that sit inside a function/method body become CALLS edges. A
  // call at file scope (module-level invocation) or inside a class/type scope
  // has no enclosing function and is dropped, matching Graphify's function-body
  // seeding.
  if (!child_function_scope.empty() && contains_symbol(config.symbols.call_nodes, symbol)) {
    add_raw_call(node, config, context, child_function_scope, raw_calls);
  }
  if (config.extra_walk) {
    config.extra_walk(node, context, fragment, raw_calls);
  }

  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index) {
    walk_node(ts_node_child(node, index), config, context, child_scope, child_kind, child_function_scope, fragment, raw_calls, raw_relations, depth + 1);
  }
}

}  // namespace

ExtractionResult extract_with_config(
    const TSLanguage* language,
    const LanguageConfig& config,
    const ExtractionContext& context) {
  ExtractionResult result;
  auto* parser = thread_parser_cache().parser_for(language);
  if (parser == nullptr) {
    result.fragment.warnings.push_back("failed to initialize tree-sitter parser");
    return result;
  }

  TreePtr tree(ts_parser_parse_string(
      parser,
      nullptr,
      context.source.data(),
      static_cast<std::uint32_t>(context.source.size())));
  if (tree == nullptr) {
    result.fragment.warnings.push_back("tree-sitter parser returned no tree");
    return result;
  }

  // Every file becomes a node so its declarations attach to a stable root via
  // `contains` edges, matching Graphify's file-rooted structure. Labelled with
  // the path tail (parent dir + filename) so distinct files that share a
  // basename are not collapsed by semantic dedup.
  const std::filesystem::path source_path(context.source_file);
  std::string file_label = source_path.filename().string();
  if (source_path.has_parent_path() && source_path.parent_path().has_filename()) {
    file_label = source_path.parent_path().filename().string() + "/" + file_label;
  }
  const auto file_id = make_id(context.source_file);
  result.fragment.nodes.push_back(Node{
      .id = file_id,
      .label = file_label.empty() ? context.source_file : std::move(file_label),
      .source_file = context.source_file,
      .source_location = SourceLocation{.start_line = 1, .end_line = 1},
      .kind = "file",
      .confidence = Confidence::Extracted,
  });

  // The file is the structural root (owns `contains` edges) but not a call
  // scope, so the function-scope seed is empty: top-level calls are dropped
  // until the walk enters a function body.
  walk_node(ts_tree_root_node(tree.get()), config, context, file_id, "file", /*function_scope_id=*/{}, result.fragment, result.raw_calls, result.raw_relations, /*depth=*/0);
  return result;
}

}  // namespace cgraph
