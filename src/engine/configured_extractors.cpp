#include "cgraph/configured_extractors.hpp"

#include "cgraph/cpp_extractor.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/non_grammar_extractors.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <cstdint>
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

[[nodiscard]] LanguageConfig java_config() {
  return LanguageConfig{
      .name = "java",
      .grammar_name = "tree-sitter-java",
      .extensions = {".java"},
      .class_node_types = {"class_declaration", "interface_declaration", "enum_declaration", "record_declaration"},
      .function_node_types = {"method_declaration", "constructor_declaration"},
      .import_node_types = {"import_declaration"},
      .call_node_types = {"method_invocation", "object_creation_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"name"},
  };
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

[[nodiscard]] LanguageConfig kotlin_config() {
  return LanguageConfig{
      .name = "kotlin",
      .grammar_name = "tree-sitter-kotlin",
      .extensions = {".kt", ".kts"},
      .class_node_types = {"class_declaration", "object_declaration", "interface_declaration"},
      .function_node_types = {"function_declaration"},
      .import_node_types = {"import_header"},
      .call_node_types = {"call_expression"},
      .name_fields = {"name"},
      .body_fields = {"body"},
      .call_accessor_fields = {"function"},
  };
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
      .source = make_id(context.source_file),
      .target = module_id,
      .relation = "imports",
      .confidence = Confidence::Extracted,
  });
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
    std::vector<std::string> segments, bool module_only, const ExtractionContext& context, Fragment& fragment) {
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
  const auto exists = std::ranges::any_of(
      fragment.nodes, [&](const Node& existing) { return existing.id == stub_id; });
  if (!exists) {
    fragment.nodes.push_back(Node{
        .id = stub_id,
        .label = label,
        .source_location = SourceLocation{.start_line = 1, .end_line = 1},
        .kind = as_module ? "module" : "import",
        .confidence = Confidence::Extracted,
        .properties = {{"import_path", joined}, {"module_layout", "rust"}},
    });
  }
  fragment.edges.push_back(Edge{
      .source = make_id(context.source_file),
      .target = stub_id,
      .relation = "imports",
      .confidence = Confidence::Extracted,
  });
}

void rust_walk_use_tree(
    const TSNode& node,
    const ExtractionContext& context,
    Fragment& fragment,
    const std::vector<std::string>& prefix) {
  const std::string_view type = ts_node_type(node);
  if (type == "identifier" || type == "scoped_identifier" || type == "crate" ||
      type == "self" || type == "super") {
    auto segments = prefix;
    if (rust_append_use_path(node, context, segments)) {
      rust_emit_use_stub(std::move(segments), false, context, fragment);
    }
    return;
  }
  if (type == "use_as_clause") {
    // Resolution goes through the ORIGINAL name; the alias never becomes a node
    // (resolve_imports remaps by the name declared in the target file).
    const auto path = ts_node_child_by_field_name(node, "path", 4);
    if (!ts_node_is_null(path)) {
      rust_walk_use_tree(path, context, fragment, prefix);
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
      rust_walk_use_tree(list, context, fragment, segments);
    }
    return;
  }
  if (type == "use_list") {
    const auto count = ts_node_named_child_count(node);
    for (std::uint32_t index = 0; index < count; ++index) {
      rust_walk_use_tree(ts_node_named_child(node, index), context, fragment, prefix);
    }
    return;
  }
  if (type == "use_wildcard") {
    auto segments = prefix;
    if (ts_node_named_child_count(node) > 0 &&
        !rust_append_use_path(ts_node_named_child(node, 0), context, segments)) {
      return;
    }
    rust_emit_use_stub(std::move(segments), true, context, fragment);
    return;
  }
  // metavariable and anything else: dropped rather than guessed.
}

void rust_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment) {
  if (std::string_view(ts_node_type(node)) != "use_declaration") {
    return;
  }
  const auto argument = ts_node_child_by_field_name(node, "argument", 8);
  if (ts_node_is_null(argument)) {
    return;
  }
  rust_walk_use_tree(argument, context, fragment, {});
}

[[nodiscard]] LanguageConfig rust_config() {
  LanguageConfig config{
      .name = "rust",
      .grammar_name = "tree-sitter-rust",
      .extensions = {".rs"},
      // No class kind in Rust: struct/enum/union/trait/type-alias are all "type"
      // nodes (mirrors Go's type_spec choice). impl blocks carry no name and are
      // deliberately not registered -- methods inside them are function_items and
      // are captured as file-contained functions, exactly like Go's methods.
      .function_node_types = {"function_item", "function_signature_item"},
      .type_node_types = {"struct_item", "enum_item", "union_item", "trait_item", "type_item"},
      .import_node_types = {"use_declaration"},
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

std::optional<ExtractionResult> extract_configured_language(
    DetectedLanguage language,
    const ExtractionContext& context) {
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
