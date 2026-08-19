#include "cgraph/javascript_extractor.hpp"

#include "cgraph/normalize.hpp"

#include <filesystem>
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

// Names an arrow function / function expression from the construct it is bound
// to (`const Foo = () => {}`, `{ handler: () => {} }`, `this.x = () => {}`,
// class fields). Returns empty for genuinely anonymous functions so the caller
// skips them. Named function declarations and methods return empty here and are
// named by the generic name_fields path instead.
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
  return {};
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

// Pushes the bare imported/exported symbol names (skipping `as` aliases) found
// under an import_clause / export_clause / namespace_(im|ex)port subtree.
void collect_specifier_names(const TSNode& node, std::string_view source, std::vector<std::string>& out) {
  const std::string_view type = ts_node_type(node);
  if (type == "import_specifier" || type == "export_specifier") {
    if (const auto name = ts_node_child_by_field_name(node, "name", 4); !ts_node_is_null(name)) {
      auto text = node_text(name, source);
      if (!text.empty()) {
        out.push_back(std::move(text));
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
        out.push_back(std::move(text));
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
  std::vector<std::string> names;
  collect_specifier_names(node, context.source, names);
  for (auto& name : names) {
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
void module_const_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment, std::vector<RawCall>&) {
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
    fragment.edges.push_back(Edge{
        .source = file_id,
        .target = std::move(id),
        .relation = "contains",
        .confidence = Confidence::Extracted,
    });
  }
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
void ts_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id, std::vector<RawRelation>& out) {
  const std::string_view node_type = ts_node_type(node);
  const bool is_class = node_type == "class_declaration" || node_type == "abstract_class_declaration";
  const bool is_interface = node_type == "interface_declaration";
  if (!is_class && !is_interface) {
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
      .extra_walk = module_const_handler,
      .relation_handler = ts_relation_handler,
  };
}

}  // namespace

LanguageConfig javascript_language_config() {
  return base_config("javascript", "tree-sitter-javascript", {".js", ".jsx", ".mjs", ".cjs"});
}

LanguageConfig typescript_language_config() {
  auto config = base_config("typescript", "tree-sitter-typescript", {".ts"});
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
