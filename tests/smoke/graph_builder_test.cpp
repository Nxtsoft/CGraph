#include "cgraph/graph_builder.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>

namespace {

[[nodiscard]] bool has_edge(const cgraph::GraphSnapshot& graph, const std::string& source, const std::string& target, const std::string& relation) {
  return std::ranges::any_of(graph.edges, [&](const cgraph::Edge& e) {
    return e.source == source && e.target == target && e.relation == relation;
  });
}

[[nodiscard]] bool has_node(const cgraph::GraphSnapshot& graph, const std::string& id) {
  return std::ranges::any_of(graph.nodes, [&](const cgraph::Node& n) { return n.id == id; });
}

// resolve_imports must relink module/import stubs onto the real file and declared
// symbol they refer to, drop the stubs, and leave third-party (unresolvable)
// imports untouched.
int test_resolve_imports() {
  const auto importer = cgraph::make_id("/proj/app/a.ts");
  const auto imported_file = cgraph::make_id("/proj/lib/foo.ts");
  const auto real_symbol = cgraph::make_id("/proj/lib/foo.ts:Foo");
  const auto module_stub = cgraph::make_id("/proj/lib/foo");
  const auto symbol_stub = cgraph::make_id("/proj/lib/foo:Foo");
  const auto external_stub = cgraph::make_id("react:useState");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = importer, .label = "app/a.ts", .source_file = "/proj/app/a.ts", .kind = "file"});
  graph.nodes.push_back({.id = imported_file, .label = "lib/foo.ts", .source_file = "/proj/lib/foo.ts", .kind = "file"});
  graph.nodes.push_back({.id = real_symbol, .label = "Foo", .source_file = "/proj/lib/foo.ts", .kind = "function"});
  graph.nodes.push_back({.id = module_stub, .label = "../lib/foo", .kind = "module", .properties = {{"import_path", "/proj/lib/foo"}}});
  graph.nodes.push_back({.id = symbol_stub, .label = "Foo", .kind = "import", .properties = {{"import_path", "/proj/lib/foo"}}});
  graph.nodes.push_back({.id = external_stub, .label = "useState", .kind = "import", .properties = {{"import_path", "react"}}});
  graph.edges.push_back({.source = importer, .target = module_stub, .relation = "imports_from"});
  graph.edges.push_back({.source = importer, .target = symbol_stub, .relation = "imports"});
  graph.edges.push_back({.source = importer, .target = external_stub, .relation = "imports"});

  cgraph::resolve_imports(graph);

  // Resolvable stubs collapse onto the real file / declared symbol.
  if (has_node(graph, module_stub) || has_node(graph, symbol_stub)) {
    return 1;
  }
  if (!has_edge(graph, importer, imported_file, "imports_from")) {
    return 1;  // module stub -> real file
  }
  if (!has_edge(graph, importer, real_symbol, "imports")) {
    return 1;  // symbol stub -> real declared symbol
  }
  // Third-party import resolves to no project file: Graphify never materialises
  // these, so the stub and its edge must be dropped.
  if (has_node(graph, external_stub)) {
    return 1;
  }
  if (has_edge(graph, importer, external_stub, "imports")) {
    return 1;
  }
  return 0;
}

// Rust stubs carry module_layout=rust: the import_path resolves as a module
// file (<path>.rs or <path>/mod.rs, unique suffix); when the full path names no
// module file, the parent path is retried as the module and the leaf as an item
// declared in it (falling back to the file node when the item is not declared).
// Zero or multiple candidate files leave no stub and no dangling edge.
int test_resolve_rust_imports() {
  const auto importer = cgraph::make_id("src/main.rs");
  const auto bar_file = cgraph::make_id("src/foo/bar.rs");
  const auto baz_symbol = cgraph::make_id("src/foo/bar.rs:Baz");
  const auto qux_mod_file = cgraph::make_id("src/qux/mod.rs");
  const auto q_symbol = cgraph::make_id("src/qux/mod.rs:Q");
  const auto util_a = cgraph::make_id("src/a/util.rs");
  const auto util_b = cgraph::make_id("src/b/util.rs");

  const auto item_stub = cgraph::make_id("rust_use:foo/bar/Baz:Baz");
  const auto module_stub = cgraph::make_id("rust_use:foo/bar:bar");
  const auto glob_stub = cgraph::make_id("rust_use:foo/bar:foo/bar");
  const auto mod_rs_stub = cgraph::make_id("rust_use:qux/Q:Q");
  const auto undeclared_stub = cgraph::make_id("rust_use:qux/Missing:Missing");
  const auto external_stub = cgraph::make_id("rust_use:serde/Serialize:Serialize");
  const auto ambiguous_stub = cgraph::make_id("rust_use:util/Thing:Thing");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = importer, .label = "src/main.rs", .source_file = "src/main.rs", .kind = "file"});
  graph.nodes.push_back({.id = bar_file, .label = "src/foo/bar.rs", .source_file = "src/foo/bar.rs", .kind = "file"});
  graph.nodes.push_back({.id = baz_symbol, .label = "Baz", .source_file = "src/foo/bar.rs", .kind = "type"});
  graph.nodes.push_back({.id = qux_mod_file, .label = "src/qux/mod.rs", .source_file = "src/qux/mod.rs", .kind = "file"});
  graph.nodes.push_back({.id = q_symbol, .label = "Q", .source_file = "src/qux/mod.rs", .kind = "function"});
  graph.nodes.push_back({.id = util_a, .label = "src/a/util.rs", .source_file = "src/a/util.rs", .kind = "file"});
  graph.nodes.push_back({.id = util_b, .label = "src/b/util.rs", .source_file = "src/b/util.rs", .kind = "file"});

  const cgraph::Node stubs[] = {
      // use crate::foo::bar::Baz; -> item declared in foo/bar.rs
      {.id = item_stub, .label = "Baz", .kind = "import",
       .properties = {{"import_path", "foo/bar/Baz"}, {"module_layout", "rust"}}},
      // use crate::foo::bar; -> the full path IS a module file
      {.id = module_stub, .label = "bar", .kind = "import",
       .properties = {{"import_path", "foo/bar"}, {"module_layout", "rust"}}},
      // use crate::foo::bar::*; -> glob names the module itself
      {.id = glob_stub, .label = "foo/bar", .kind = "module",
       .properties = {{"import_path", "foo/bar"}, {"module_layout", "rust"}}},
      // use crate::qux::Q; -> mod.rs layout resolves the parent module
      {.id = mod_rs_stub, .label = "Q", .kind = "import",
       .properties = {{"import_path", "qux/Q"}, {"module_layout", "rust"}}},
      // use crate::qux::Missing; -> module resolves, item is not declared -> file
      {.id = undeclared_stub, .label = "Missing", .kind = "import",
       .properties = {{"import_path", "qux/Missing"}, {"module_layout", "rust"}}},
      // use serde::Serialize; -> external crate, no project file
      {.id = external_stub, .label = "Serialize", .kind = "import",
       .properties = {{"import_path", "serde/Serialize"}, {"module_layout", "rust"}}},
      // use crate::util::Thing; -> two files match the util suffix -> ambiguous
      {.id = ambiguous_stub, .label = "Thing", .kind = "import",
       .properties = {{"import_path", "util/Thing"}, {"module_layout", "rust"}}},
  };
  for (const auto& stub : stubs) {
    graph.nodes.push_back(stub);
    graph.edges.push_back({.source = importer, .target = stub.id, .relation = "imports"});
  }

  cgraph::resolve_imports(graph);

  for (const auto& stub : stubs) {
    if (has_node(graph, stub.id)) {
      return 1;  // every stub is consumed: remapped or dropped
    }
  }
  if (!has_edge(graph, importer, baz_symbol, "imports")) {
    return 1;  // item import -> declared symbol
  }
  if (!has_edge(graph, importer, bar_file, "imports")) {
    return 1;  // module import (and glob) -> module file
  }
  if (!has_edge(graph, importer, q_symbol, "imports")) {
    return 1;  // mod.rs layout -> declared symbol in qux/mod.rs
  }
  if (!has_edge(graph, importer, qux_mod_file, "imports")) {
    return 1;  // undeclared item -> the resolved module's file node
  }
  // External and ambiguous imports leave nothing behind.
  if (has_edge(graph, importer, util_a, "imports") || has_edge(graph, importer, util_b, "imports")) {
    return 1;
  }
  for (const auto& edge : graph.edges) {
    if (edge.target == external_stub || edge.target == ambiguous_stub) {
      return 1;
    }
  }
  return 0;
}

[[nodiscard]] cgraph::Confidence edge_confidence(const cgraph::GraphSnapshot& graph, const std::string& source, const std::string& target, const std::string& relation) {
  for (const auto& e : graph.edges) {
    if (e.source == source && e.target == target && e.relation == relation) {
      return e.confidence;
    }
  }
  return cgraph::Confidence::Ambiguous;  // sentinel: no such edge (calls are only Extracted/Inferred)
}

// Call resolution mirrors Graphify: a call resolves to a same-file declaration
// (EXTRACTED) or to a project-wide *unique* label. A unique target the caller's
// file imports is EXTRACTED; one it does not import is still resolved but marked
// INFERRED. An ambiguous name (more than one declaration anywhere) and a
// built-in global resolve to nothing.
int test_call_scoping() {
  const auto a_file = cgraph::make_id("/p/a.ts");
  const auto caller = cgraph::make_id("/p/a.ts:main");
  const auto local_helper = cgraph::make_id("/p/a.ts:localHelper");
  const auto imported_helper = cgraph::make_id("/p/b.ts:helper");
  const auto orphan = cgraph::make_id("/p/c.ts:orphan");
  const auto dup_one = cgraph::make_id("/p/d.ts:dup");
  const auto dup_two = cgraph::make_id("/p/e.ts:dup");
  // A field is the only node in the project bearing this name, so a name-only
  // index would resolve a call to it. A field is not callable.
  const auto field_named_connect = cgraph::make_id("/p/f.ts:connect");
  // A class IS a callable target: `Ctor()` is a constructor call.
  const auto ctor_class = cgraph::make_id("/p/g.ts:Ctor");
  // ...and so is a module-level binding that holds a callable.
  const auto callable_binding = cgraph::make_id("/p/h.ts:boundCallable");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = a_file, .label = "a.ts", .source_file = "/p/a.ts", .kind = "file"});
  graph.nodes.push_back({.id = caller, .label = "main", .source_file = "/p/a.ts", .kind = "function"});
  graph.nodes.push_back({.id = local_helper, .label = "localHelper", .source_file = "/p/a.ts", .kind = "function"});
  graph.nodes.push_back({.id = imported_helper, .label = "helper", .source_file = "/p/b.ts", .kind = "function"});
  graph.nodes.push_back({.id = orphan, .label = "orphan", .source_file = "/p/c.ts", .kind = "function"});
  graph.nodes.push_back({.id = dup_one, .label = "dup", .source_file = "/p/d.ts", .kind = "function"});
  graph.nodes.push_back({.id = dup_two, .label = "dup", .source_file = "/p/e.ts", .kind = "function"});
  graph.nodes.push_back({.id = field_named_connect, .label = "connect", .source_file = "/p/f.ts", .kind = "field"});
  graph.nodes.push_back({.id = ctor_class, .label = "Ctor", .source_file = "/p/g.ts", .kind = "class"});
  graph.nodes.push_back({.id = callable_binding, .label = "boundCallable", .source_file = "/p/h.ts", .kind = "variable"});
  // a.ts imports `helper` -> the resolved call to it should be EXTRACTED.
  graph.edges.push_back({.source = a_file, .target = imported_helper, .relation = "imports"});

  const cgraph::RawCall calls[] = {
      {.caller_id = caller, .callee_label = "localHelper", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "helper", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "orphan", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "dup", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "Map", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "connect", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "Ctor", .source_file = "/p/a.ts"},
      {.caller_id = caller, .callee_label = "boundCallable", .source_file = "/p/a.ts"},
  };
  cgraph::resolve_imports(graph);
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  // Every call site this resolver is answerable for lands in exactly one outcome.
  // Without this, a future resolution path could be added without being counted
  // and the reported rate would quietly stop meaning anything.
  if (!outcomes.balances()) {
    return 1;
  }
  // The fixture's counted calls: localHelper (same file); helper, Ctor,
  // boundCallable and orphan (project-unique, the last unimported); dup
  // (ambiguous); connect (a field, so not callable -> unknown). `Map` is a
  // built-in and is never counted.
  if (outcomes.total != 7) {
    return 1;
  }
  if (outcomes.resolved_same_file != 1 || outcomes.resolved_project_unique != 4) {
    return 1;
  }
  if (outcomes.dropped_ambiguous != 1 || outcomes.dropped_unknown != 1) {
    return 1;
  }

  // Same-file declaration resolves with EXTRACTED confidence.
  if (edge_confidence(graph, caller, local_helper, "CALLS") != cgraph::Confidence::Extracted) {
    return 1;
  }
  // Globally unique + imported -> EXTRACTED.
  if (edge_confidence(graph, caller, imported_helper, "CALLS") != cgraph::Confidence::Extracted) {
    return 1;
  }
  // Globally unique but not imported -> resolves, but INFERRED.
  if (edge_confidence(graph, caller, orphan, "CALLS") != cgraph::Confidence::Inferred) {
    return 1;
  }
  // Ambiguous name (two declarations) resolves to nothing.
  if (has_edge(graph, caller, dup_one, "CALLS") || has_edge(graph, caller, dup_two, "CALLS")) {
    return 1;
  }
  // A call target must be callable. A uniquely-named FIELD is not: on the real
  // repo this is how `unix_endpoint_is_live` came to "call" a struct member named
  // `connect` while actually invoking the ::connect syscall, which then showed up
  // as a false dependent in `impact`.
  if (has_edge(graph, caller, field_named_connect, "CALLS")) {
    return 1;
  }
  // A CLASS is callable, because `Ctor()` is a constructor call. The eligible set
  // is the same one the per-file table admits (function/class/type/variable), so
  // the two resolution tiers agree on what a symbol is; only `field` is excluded.
  if (!has_edge(graph, caller, ctor_class, "CALLS")) {
    return 1;
  }
  if (!has_edge(graph, caller, callable_binding, "CALLS")) {
    return 1;  // a module-level binding can hold a callable
  }
  // A built-in global (`Map`) is never wired to a project node, even though no
  // project node named Map exists here — the call is simply dropped.
  return 0;
}

// Heritage resolves through imports AND a same-file declaration; references
// resolve only through imports (a same-file reference is dropped, matching
// Graphify). Unresolvable targets emit no edge.
int test_resolve_relations() {
  const auto a_file = cgraph::make_id("/p/a.ts");
  const auto svc = cgraph::make_id("/p/a.ts:Service");
  const auto imported_base = cgraph::make_id("/p/base.ts:Base");
  const auto local_mixin = cgraph::make_id("/p/a.ts:Mixin");
  const auto imported_cfg = cgraph::make_id("/p/config.ts:Config");
  const auto same_file_type = cgraph::make_id("/p/a.ts:Local");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = a_file, .label = "a.ts", .source_file = "/p/a.ts", .kind = "file"});
  graph.nodes.push_back({.id = svc, .label = "Service", .source_file = "/p/a.ts", .kind = "class"});
  graph.nodes.push_back({.id = imported_base, .label = "Base", .source_file = "/p/base.ts", .kind = "class"});
  graph.nodes.push_back({.id = local_mixin, .label = "Mixin", .source_file = "/p/a.ts", .kind = "class"});
  graph.nodes.push_back({.id = imported_cfg, .label = "Config", .source_file = "/p/config.ts", .kind = "type"});
  graph.nodes.push_back({.id = same_file_type, .label = "Local", .source_file = "/p/a.ts", .kind = "type"});
  // a.ts imports Base and Config.
  graph.edges.push_back({.source = a_file, .target = imported_base, .relation = "imports"});
  graph.edges.push_back({.source = a_file, .target = imported_cfg, .relation = "imports"});

  const cgraph::RawRelation relations[] = {
      {.source_id = svc, .target_label = "Base", .relation = "inherits", .context = "type", .source_file = "/p/a.ts", .allow_same_file = true},
      {.source_id = svc, .target_label = "Mixin", .relation = "implements", .context = "type", .source_file = "/p/a.ts", .allow_same_file = true},
      {.source_id = svc, .target_label = "Config", .relation = "references", .context = "parameter_type", .source_file = "/p/a.ts", .allow_same_file = false},
      {.source_id = svc, .target_label = "Local", .relation = "references", .context = "field", .source_file = "/p/a.ts", .allow_same_file = false},
      {.source_id = svc, .target_label = "Nowhere", .relation = "references", .context = "field", .source_file = "/p/a.ts", .allow_same_file = false},
  };
  cgraph::resolve_raw_relations(graph, relations);

  if (!has_edge(graph, svc, imported_base, "inherits")) {
    return 1;  // heritage via import
  }
  if (!has_edge(graph, svc, local_mixin, "implements")) {
    return 1;  // heritage via same-file declaration
  }
  if (!has_edge(graph, svc, imported_cfg, "references")) {
    return 1;  // reference via import
  }
  if (has_edge(graph, svc, same_file_type, "references")) {
    return 1;  // same-file reference must NOT resolve
  }
  return 0;
}

}  // namespace

int main() {
  if (test_resolve_rust_imports() != 0) {
    return 1;
  }
  if (test_resolve_imports() != 0) {
    return 1;
  }
  if (test_call_scoping() != 0) {
    return 1;
  }
  if (test_resolve_relations() != 0) {
    return 1;
  }

  cgraph::Fragment first;
  first.nodes.push_back(cgraph::Node{.id = "a", .label = "Alpha", .source_file = "a.cpp", .kind = "function"});
  first.nodes.push_back(cgraph::Node{.id = "a", .label = "Alpha duplicate", .source_file = "a.cpp", .kind = "function"});
  first.nodes.push_back(cgraph::Node{.id = "b", .label = "Beta", .source_file = "b.cpp", .kind = "function"});
  first.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "USES"});
  first.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "USES"});

  first.hyperedges.push_back(cgraph::Hyperedge{.id = "h1", .nodes = {"a", "b"}, .relation = "GROUP"});

  cgraph::Fragment second;
  second.nodes.push_back(cgraph::Node{.id = "b", .label = "Beta", .source_file = "b.cpp", .kind = "function"});
  second.nodes.push_back(cgraph::Node{.id = "c", .label = "Gamma", .source_file = "c.cpp", .kind = "function"});
  // Cross-fragment duplicate hyperedge id: the bulk merge must dedup it the same
  // way it dedups nodes and edges (regression guard for the index-maintained merge).
  second.hyperedges.push_back(cgraph::Hyperedge{.id = "h1", .nodes = {"a", "b"}, .relation = "GROUP"});

  const cgraph::Fragment fragments[] = {first, second};
  auto graph = cgraph::merge_fragments(fragments);
  if (graph.nodes.size() != 3 || graph.edges.size() != 1 || graph.hyperedges.size() != 1) {
    return 1;
  }
  // First occurrence wins: the duplicate "a" (label "Alpha duplicate") must not
  // overwrite the original. A last-wins regression would pass the count check above.
  for (const auto& node : graph.nodes) {
    if (node.id == "a" && node.label != "Alpha") {
      return 1;
    }
  }

  const cgraph::RawCall calls[] = {
      {.caller_id = "a", .callee_label = "Gamma", .source_file = "a.cpp"},
      {.caller_id = "a", .callee_label = "log", .source_file = "a.cpp"},
  };
  cgraph::resolve_raw_calls(graph, calls);
  if (graph.edges.size() != 2) {
    return 1;
  }
  if (graph.edges.back().relation != "CALLS" || graph.edges.back().confidence != cgraph::Confidence::Inferred) {
    return 1;
  }

  // Regression: a call whose caller is not a real graph node must be dropped,
  // not attached to a synthetic file id. A dangling edge (source missing from
  // the node set) is unrenderable by any consumer.
  const cgraph::RawCall dangling[] = {
      {.caller_id = "", .callee_label = "Gamma", .source_file = "a.cpp"},
      {.caller_id = "missing-node", .callee_label = "Gamma", .source_file = "a.cpp"},
  };
  cgraph::resolve_raw_calls(graph, dangling);
  if (graph.edges.size() != 2) {
    return 1;  // no new edges should have been created
  }

  // Regression: a node must not call itself (caller resolves to the callee).
  const cgraph::RawCall self_call[] = {
      {.caller_id = "a", .callee_label = "Alpha", .source_file = "a.cpp"},
  };
  cgraph::resolve_raw_calls(graph, self_call);
  if (graph.edges.size() != 2) {
    return 1;  // self-edge must be skipped
  }

  return 0;
}
