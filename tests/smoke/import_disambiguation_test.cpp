#include "cgraph/graph_builder.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <string>

namespace {

[[nodiscard]] bool has_edge(const cgraph::GraphSnapshot& graph, const std::string& source,
                            const std::string& target, const std::string& relation) {
  return std::ranges::any_of(graph.edges, [&](const cgraph::Edge& e) {
    return e.source == source && e.target == target && e.relation == relation;
  });
}

[[nodiscard]] cgraph::Confidence edge_confidence(const cgraph::GraphSnapshot& graph,
                                                 const std::string& source,
                                                 const std::string& target,
                                                 const std::string& relation) {
  const auto found = std::ranges::find_if(graph.edges, [&](const cgraph::Edge& e) {
    return e.source == source && e.target == target && e.relation == relation;
  });
  return found == graph.edges.end() ? cgraph::Confidence::Inferred : found->confidence;
}

// A name declared in two files is ambiguous — unless the caller's file imports
// one of them by name, which says outright which declaration it meant. The
// import is evidence, so the call resolves to it and is graded EXTRACTED.
int test_import_breaks_the_tie() {
  const auto caller_file = cgraph::make_id("/p/report.py");
  const auto storage_file = cgraph::make_id("/p/storage.py");
  const auto cache_file = cgraph::make_id("/p/cache.py");
  const auto render = cgraph::make_id("/p/report.py:render");
  const auto storage_write = cgraph::make_id("/p/storage.py:write_text");
  const auto cache_write = cgraph::make_id("/p/cache.py:write_text");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = caller_file, .label = "report.py", .source_file = "/p/report.py", .kind = "file"});
  graph.nodes.push_back({.id = storage_file, .label = "storage.py", .source_file = "/p/storage.py", .kind = "file"});
  graph.nodes.push_back({.id = cache_file, .label = "cache.py", .source_file = "/p/cache.py", .kind = "file"});
  graph.nodes.push_back({.id = render, .label = "render", .source_file = "/p/report.py", .kind = "function"});
  graph.nodes.push_back({.id = storage_write, .label = "write_text", .source_file = "/p/storage.py", .kind = "function"});
  graph.nodes.push_back({.id = cache_write, .label = "write_text", .source_file = "/p/cache.py", .kind = "function"});
  // `from storage import write_text` — names the declaration outright.
  graph.edges.push_back({.source = caller_file, .target = storage_write, .relation = "imports"});

  const cgraph::RawCall calls[] = {
      {.caller_id = render, .callee_label = "write_text", .source_file = "/p/report.py"},
  };
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  if (!outcomes.balances() || outcomes.total != 1) {
    return 1;
  }
  // Resolved, not dropped — and to the imported declaration, never the other one.
  if (outcomes.dropped_ambiguous != 0 || outcomes.resolved_project_unique != 1) {
    return 1;
  }
  if (!has_edge(graph, render, storage_write, "CALLS")) {
    return 1;
  }
  if (has_edge(graph, render, cache_write, "CALLS")) {
    return 1;
  }
  // The import is what proved it, so the edge carries the same confidence a
  // project-unique imported name would.
  if (edge_confidence(graph, render, storage_write, "CALLS") != cgraph::Confidence::Extracted) {
    return 1;
  }
  return 0;
}

// The rule this narrows must otherwise hold exactly as before: a caller with no
// import evidence still cannot choose between two declarations of one name.
int test_without_an_import_it_stays_ambiguous() {
  const auto caller_file = cgraph::make_id("/p/audit.py");
  const auto sweep = cgraph::make_id("/p/audit.py:sweep");
  const auto storage_write = cgraph::make_id("/p/storage.py:write_text");
  const auto cache_write = cgraph::make_id("/p/cache.py:write_text");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = caller_file, .label = "audit.py", .source_file = "/p/audit.py", .kind = "file"});
  graph.nodes.push_back({.id = sweep, .label = "sweep", .source_file = "/p/audit.py", .kind = "function"});
  graph.nodes.push_back({.id = storage_write, .label = "write_text", .source_file = "/p/storage.py", .kind = "function"});
  graph.nodes.push_back({.id = cache_write, .label = "write_text", .source_file = "/p/cache.py", .kind = "function"});

  const cgraph::RawCall calls[] = {
      {.caller_id = sweep, .callee_label = "write_text", .source_file = "/p/audit.py"},
  };
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  if (!outcomes.balances() || outcomes.total != 1) {
    return 1;
  }
  if (outcomes.dropped_ambiguous != 1) {
    return 1;
  }
  if (has_edge(graph, sweep, storage_write, "CALLS") || has_edge(graph, sweep, cache_write, "CALLS")) {
    return 1;
  }
  return 0;
}

// Importing BOTH declarations of a name picks neither. An import that names two
// candidates is not evidence for either one, so the call stays dropped rather
// than resolving to whichever happens to be first.
int test_two_imports_of_one_name_stay_ambiguous() {
  const auto caller_file = cgraph::make_id("/p/report.py");
  const auto render = cgraph::make_id("/p/report.py:render");
  const auto storage_write = cgraph::make_id("/p/storage.py:write_text");
  const auto cache_write = cgraph::make_id("/p/cache.py:write_text");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = caller_file, .label = "report.py", .source_file = "/p/report.py", .kind = "file"});
  graph.nodes.push_back({.id = render, .label = "render", .source_file = "/p/report.py", .kind = "function"});
  graph.nodes.push_back({.id = storage_write, .label = "write_text", .source_file = "/p/storage.py", .kind = "function"});
  graph.nodes.push_back({.id = cache_write, .label = "write_text", .source_file = "/p/cache.py", .kind = "function"});
  graph.edges.push_back({.source = caller_file, .target = storage_write, .relation = "imports"});
  graph.edges.push_back({.source = caller_file, .target = cache_write, .relation = "imports"});

  const cgraph::RawCall calls[] = {
      {.caller_id = render, .callee_label = "write_text", .source_file = "/p/report.py"},
  };
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  if (!outcomes.balances() || outcomes.dropped_ambiguous != 1) {
    return 1;
  }
  if (has_edge(graph, render, storage_write, "CALLS") || has_edge(graph, render, cache_write, "CALLS")) {
    return 1;
  }
  return 0;
}

// When the imported declarations are an overload set — several signatures of one
// name in ONE file — issue #52's rule applies unchanged: edge to every member,
// INFERRED, because any of them may be the callee. The unrelated third-file
// declaration of the same name is excluded by the import filter.
int test_imported_overload_set_edges_to_every_member() {
  const auto caller_file = cgraph::make_id("/p/Main.java");
  const auto call_site = cgraph::make_id("/p/Main.java:run");
  const auto add_int = cgraph::make_id("/p/Sum.java:add#1");
  const auto add_str = cgraph::make_id("/p/Sum.java:add#2");
  const auto unrelated_add = cgraph::make_id("/p/Other.java:add");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = caller_file, .label = "Main.java", .source_file = "/p/Main.java", .kind = "file"});
  graph.nodes.push_back({.id = call_site, .label = "run", .source_file = "/p/Main.java", .kind = "function"});
  graph.nodes.push_back({.id = add_int, .label = "add", .source_file = "/p/Sum.java", .kind = "function"});
  graph.nodes.push_back({.id = add_str, .label = "add", .source_file = "/p/Sum.java", .kind = "function"});
  graph.nodes.push_back({.id = unrelated_add, .label = "add", .source_file = "/p/Other.java", .kind = "function"});
  graph.edges.push_back({.source = caller_file, .target = add_int, .relation = "imports"});
  graph.edges.push_back({.source = caller_file, .target = add_str, .relation = "imports"});

  const cgraph::RawCall calls[] = {
      {.caller_id = call_site, .callee_label = "add", .source_file = "/p/Main.java"},
  };
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  if (!outcomes.balances() || outcomes.dropped_ambiguous != 0) {
    return 1;
  }
  if (outcomes.resolved_overload_first != 1) {
    return 1;
  }
  // Both members of the imported overload set, so a change in either reaches the
  // caller in a reverse-dependency walk.
  if (!has_edge(graph, call_site, add_int, "CALLS") || !has_edge(graph, call_site, add_str, "CALLS")) {
    return 1;
  }
  // The declaration this file did not import stays unreachable.
  if (has_edge(graph, call_site, unrelated_add, "CALLS")) {
    return 1;
  }
  return 0;
}

// A re-export carries the same proof as a direct import: `imports` and
// `re_exports` both mean the caller's file named this declaration.
int test_re_export_edge_also_breaks_the_tie() {
  const auto caller_file = cgraph::make_id("/p/app.ts");
  const auto use_it = cgraph::make_id("/p/app.ts:useIt");
  const auto lib_parse = cgraph::make_id("/p/lib.ts:parse");
  const auto vendor_parse = cgraph::make_id("/p/vendor.ts:parse");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = caller_file, .label = "app.ts", .source_file = "/p/app.ts", .kind = "file"});
  graph.nodes.push_back({.id = use_it, .label = "useIt", .source_file = "/p/app.ts", .kind = "function"});
  graph.nodes.push_back({.id = lib_parse, .label = "parse", .source_file = "/p/lib.ts", .kind = "function"});
  graph.nodes.push_back({.id = vendor_parse, .label = "parse", .source_file = "/p/vendor.ts", .kind = "function"});
  graph.edges.push_back({.source = caller_file, .target = lib_parse, .relation = "re_exports"});

  const cgraph::RawCall calls[] = {
      {.caller_id = use_it, .callee_label = "parse", .source_file = "/p/app.ts"},
  };
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  if (!outcomes.balances() || outcomes.dropped_ambiguous != 0) {
    return 1;
  }
  if (!has_edge(graph, use_it, lib_parse, "CALLS") || has_edge(graph, use_it, vendor_parse, "CALLS")) {
    return 1;
  }
  return 0;
}

// A module-level import does not name a declaration, so it must not break a tie.
// `import storage` followed by a BARE `write_text()` is not a call this tier can
// prove: the spelling that the module import would justify is `storage.write_text()`,
// a member call resolution never routes here.
int test_module_import_alone_does_not_break_the_tie() {
  const auto caller_file = cgraph::make_id("/p/report.py");
  const auto storage_file = cgraph::make_id("/p/storage.py");
  const auto render = cgraph::make_id("/p/report.py:render");
  const auto storage_write = cgraph::make_id("/p/storage.py:write_text");
  const auto cache_write = cgraph::make_id("/p/cache.py:write_text");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back({.id = caller_file, .label = "report.py", .source_file = "/p/report.py", .kind = "file"});
  graph.nodes.push_back({.id = storage_file, .label = "storage.py", .source_file = "/p/storage.py", .kind = "file"});
  graph.nodes.push_back({.id = render, .label = "render", .source_file = "/p/report.py", .kind = "function"});
  graph.nodes.push_back({.id = storage_write, .label = "write_text", .source_file = "/p/storage.py", .kind = "function"});
  graph.nodes.push_back({.id = cache_write, .label = "write_text", .source_file = "/p/cache.py", .kind = "function"});
  graph.edges.push_back({.source = caller_file, .target = storage_file, .relation = "imports_from"});

  const cgraph::RawCall calls[] = {
      {.caller_id = render, .callee_label = "write_text", .source_file = "/p/report.py"},
  };
  cgraph::CallResolution outcomes;
  cgraph::resolve_raw_calls(graph, calls, &outcomes);

  if (!outcomes.balances() || outcomes.dropped_ambiguous != 1) {
    return 1;
  }
  if (has_edge(graph, render, storage_write, "CALLS")) {
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  if (test_import_breaks_the_tie() != 0) {
    return 1;
  }
  if (test_without_an_import_it_stays_ambiguous() != 0) {
    return 1;
  }
  if (test_two_imports_of_one_name_stay_ambiguous() != 0) {
    return 1;
  }
  if (test_imported_overload_set_edges_to_every_member() != 0) {
    return 1;
  }
  if (test_re_export_edge_also_breaks_the_tie() != 0) {
    return 1;
  }
  if (test_module_import_alone_does_not_break_the_tie() != 0) {
    return 1;
  }
  return 0;
}
