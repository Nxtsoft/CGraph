#include "cgraph/configured_extractors.hpp"

#include "cgraph/cpp_extractor.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/non_grammar_extractors.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Defined below (next to go_import_handler); forward-declared so the Java callee
// resolver above it can reuse the same byte-safe node-text helper.
[[nodiscard]] std::string go_node_text(const TSNode& node, std::string_view source);

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
  };
  config.resolve_callee_name = java_callee_name;
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

// Materializes Go interface method sets: each `method_elem` of an
// `interface_type` becomes a function node (tagged interface_method) owned by
// the interface's type node via a `method` edge, so dispatch resolution can
// see what an interface promises. The node id is namespaced — an interface
// method is a contract entry, never the same node as an implementation.
void go_extra_walk(const TSNode& node, const ExtractionContext& context,
                   const std::string& /*function_scope_id*/, Fragment& fragment,
                   std::vector<RawCall>& raw_calls) {
  (void)raw_calls;
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
  const auto interface_id = make_id(context.source_file + ":" + interface_name);

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
        .id = make_id("iface-method:" + context.source_file + ":" + interface_name + ":" + method_name),
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
  const auto exists = std::ranges::any_of(
      fragment.nodes, [&](const Node& existing) { return existing.id == stub_id; });
  if (!exists) {
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
      .source = make_id(context.source_file),
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
                     std::vector<RawCall>& raw_calls) {
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
  const auto trait_id = make_id(context.source_file + ":" + trait_name);
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
        .id = make_id("trait-method:" + context.source_file + ":" + trait_name + ":" + method_name),
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
