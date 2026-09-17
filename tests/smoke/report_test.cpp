// report.cpp: the `report` op and its modules view -- grouping by directory
// depth, cross-module edge aggregation, longest-path layering, cycle listing,
// scope and test-root filtering, whole-row budget shedding with `omitted`, the
// four renderers, and the daemon envelope (reserved views, mistyped params,
// the upgrade hint for a graphd that predates the op) -- its types view:
// identical shapes grouped, duplicate names across files, subset / overlapping
// member sets, unreferenced types, the member floor, and its own shedding order
// -- its clones view: fingerprint Jaccard classes, the token floor, the
// test-class bucket, the missing-fingerprint hint, and shedding -- and its
// design view: entry-point kinds, reach ranking, bounded flows, layers,
// unreached functions, and shedding.
#include "cgraph/report.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

int fail(const std::string& what) {
  std::cerr << "report_test: " << what << '\n';
  return 1;
}

cgraph::Node file_node(const std::string& path) {
  return cgraph::Node{.id = "file:" + path, .label = path, .source_file = "/proj/" + path, .kind = "file"};
}

cgraph::Node symbol(const std::string& path, const std::string& name) {
  return cgraph::Node{.id = path + ":" + name, .label = name, .source_file = "/proj/" + path, .kind = "function"};
}

cgraph::Edge edge(const std::string& source, const std::string& target, const char* relation) {
  return cgraph::Edge{.source = source, .target = target, .relation = relation};
}

// a/x (2 files) and a/y (1 file) under package a, b (1 file), tests (1 file).
// b -> a/x -> a/y is the dependency chain; tests hammers a/x.
cgraph::GraphSnapshot fixture() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  for (const char* path : {"a/x/f1.py", "a/x/f2.py", "a/y/g.py", "b/h.py", "tests/t.py"}) {
    graph.nodes.push_back(file_node(path));
  }
  graph.nodes.push_back(symbol("a/x/f1.py", "run"));
  graph.nodes.push_back(symbol("a/x/f2.py", "helper"));
  graph.nodes.push_back(symbol("a/y/g.py", "store"));
  graph.nodes.push_back(symbol("b/h.py", "main"));
  graph.nodes.push_back(symbol("tests/t.py", "test_run"));
  // Enrichment prose never joins a module.
  graph.nodes.push_back(cgraph::Node{.id = "doc:readme", .label = "README", .source_file = "/proj/README.md", .kind = "doc"});
  graph.edges.push_back(edge("a/x/f1.py:run", "a/y/g.py:store", "CALLS"));
  graph.edges.push_back(edge("a/x/f2.py:helper", "a/y/g.py:store", "CALLS"));
  graph.edges.push_back(edge("file:a/x/f1.py", "file:a/y/g.py", "imports"));
  graph.edges.push_back(edge("a/x/f1.py:run", "a/x/f2.py:helper", "CALLS"));  // intra-module: not an edge
  graph.edges.push_back(edge("b/h.py:main", "a/x/f1.py:run", "CALLS"));
  graph.edges.push_back(edge("file:b/h.py", "a/x/f1.py:run", "imports"));
  graph.edges.push_back(edge("file:a/x/f1.py", "a/x/f2.py:helper", "contains"));  // not a dependency relation
  for (int i = 0; i < 5; ++i) {
    graph.edges.push_back(edge("tests/t.py:test_run", "a/x/f1.py:run", "CALLS"));
  }
  graph.edges.push_back(edge("file:tests/t.py", "file:a/x/f1.py", "imports_from"));
  // A collision-resolved call INTO a test root: production code never depends
  // on a test, so this must vanish with the test root, not make it a target.
  graph.edges.push_back(edge("b/h.py:main", "tests/t.py:test_run", "CALLS"));
  return graph;
}

const cgraph::ModuleSummary* module_named(const cgraph::ModulesReport& report, const std::string& name) {
  const auto it = std::find_if(report.modules.begin(), report.modules.end(),
                               [&](const cgraph::ModuleSummary& m) { return m.name == name; });
  return it == report.modules.end() ? nullptr : &*it;
}

const cgraph::ModuleDependency* edge_named(const cgraph::ModulesReport& report, const std::string& from,
                                           const std::string& to) {
  const auto it = std::find_if(report.edges.begin(), report.edges.end(),
                               [&](const cgraph::ModuleDependency& e) { return e.from == from && e.to == to; });
  return it == report.edges.end() ? nullptr : &*it;
}

cgraph::ReportRequest request_for(int depth = 2) {
  cgraph::ReportRequest request;
  request.module_depth = depth;
  request.project_root = "/proj";
  request.budget = 0;
  return request;
}

int test_grouping_and_test_exclusion() {
  const auto graph = fixture();
  const auto report = cgraph::build_modules_report(graph, request_for());
  if (report.modules.size() != 3) {
    return fail("depth 2 without tests groups into a/x, a/y, b; got " + std::to_string(report.modules.size()));
  }
  const auto* ax = module_named(report, "a/x");
  const auto* ay = module_named(report, "a/y");
  const auto* b = module_named(report, "b");
  if (ax == nullptr || ay == nullptr || b == nullptr || module_named(report, "tests") != nullptr) {
    return fail("module set is a/x, a/y, b (tests excluded by default)");
  }
  if (ax->files != 2 || ax->symbols != 2 || ay->files != 1 || ay->symbols != 1 || b->files != 1) {
    return fail("file and symbol counts per module");
  }
  const auto* ax_ay = edge_named(report, "a/x", "a/y");
  const auto* b_ax = edge_named(report, "b", "a/x");
  if (ax_ay == nullptr || ax_ay->calls != 2 || ax_ay->imports != 1) {
    return fail("a/x -> a/y aggregates 2 calls, 1 import (intra-module and contains edges ignored)");
  }
  if (b_ax == nullptr || b_ax->calls != 1 || b_ax->imports != 1 || report.edges.size() != 2) {
    return fail("b -> a/x aggregates 1 call, 1 import; no edge from or into tests");
  }
  if (report.edges.front().from != "a/x") {
    return fail("edges are heaviest first");
  }
  if (report.total_modules != 3 || report.total_edges != 2 || report.omitted_modules != 0 || report.omitted_edges != 0) {
    return fail("totals and omitted are reported even when nothing was shed");
  }

  auto with_tests = request_for();
  with_tests.include_tests = true;
  const auto full = cgraph::build_modules_report(graph, with_tests);
  const auto* t_ax = edge_named(full, "tests", "a/x");
  if (full.modules.size() != 4 || t_ax == nullptr || t_ax->calls != 5 || t_ax->imports != 1) {
    return fail("include_tests admits tests -> a/x with 5 calls, 1 import (imports_from counts as an import)");
  }
  const auto* b_t = edge_named(full, "b", "tests");
  if (b_t == nullptr || b_t->calls != 1 || module_named(full, "tests")->layer != 1 || module_named(full, "b")->layer != 0) {
    return fail("include_tests admits tests as a target too (b -> tests), so tests sits below b");
  }
  return 0;
}

int test_layers() {
  auto graph = fixture();
  // A shortcut b -> a/y must not pull a/y up: the layer is the LONGEST path.
  graph.edges.push_back(edge("b/h.py:main", "a/y/g.py:store", "CALLS"));
  const auto report = cgraph::build_modules_report(graph, request_for());
  if (module_named(report, "b")->layer != 0 || module_named(report, "a/x")->layer != 1 ||
      module_named(report, "a/y")->layer != 2) {
    return fail("layers follow the longest dependency path: b=0, a/x=1, a/y=2");
  }
  if (report.layers.size() != 3 || report.layers[0] != std::vector<std::string>{"b"} ||
      report.layers[1] != std::vector<std::string>{"a/x"} || report.layers[2] != std::vector<std::string>{"a/y"}) {
    return fail("layers list names per layer");
  }
  if (report.modules.front().name != "b" || report.modules.back().name != "a/y") {
    return fail("modules are ordered by layer");
  }
  if (!report.cycles.empty()) {
    return fail("a DAG lists no cycles");
  }
  return 0;
}

int test_cycles() {
  auto graph = fixture();
  graph.edges.push_back(edge("a/y/g.py:store", "a/x/f2.py:helper", "CALLS"));
  const auto report = cgraph::build_modules_report(graph, request_for());
  if (report.cycles.size() != 1 || report.cycles[0] != std::vector<std::string>{"a/x", "a/y"}) {
    return fail("a/x <-> a/y is listed as one cycle");
  }
  const auto* ax_ay = edge_named(report, "a/x", "a/y");
  const auto* ay_ax = edge_named(report, "a/y", "a/x");
  const auto* b_ax = edge_named(report, "b", "a/x");
  if (ax_ay == nullptr || ay_ax == nullptr || !ax_ay->cycle || !ay_ax->cycle || b_ax == nullptr || b_ax->cycle) {
    return fail("edges inside the strongly connected component are flagged cycle; b -> a/x is not");
  }
  if (module_named(report, "b")->layer != 0 || module_named(report, "a/x")->layer != 1 ||
      module_named(report, "a/y")->layer != 1 || !module_named(report, "a/x")->in_cycle) {
    return fail("a cycle collapses to one layer so layering still terminates");
  }
  const auto mermaid = cgraph::render_modules_mermaid(report);
  if (mermaid.find("-.->") == std::string::npos || mermaid.find("%% cycle: a/x a/y") == std::string::npos) {
    return fail("mermaid draws cycle edges dashed and lists the cycle");
  }
  const auto svg = cgraph::render_modules_svg(report);
  if (svg.find("stroke-dasharray") == std::string::npos || svg.find("cycles: 1") == std::string::npos) {
    return fail("svg draws cycle edges dashed and counts cycles in the caption");
  }
  // Drawing only: the cycle's two members are spread into sub-columns (three
  // distinct box x positions for three modules, one "layer 1" caption).
  std::set<std::string> box_xs;
  for (std::size_t at = svg.find("<rect x=\""); at != std::string::npos; at = svg.find("<rect x=\"", at + 1)) {
    const auto start = at + 9;
    box_xs.insert(svg.substr(start, svg.find('"', start) - start));
  }
  std::size_t layer_captions = 0;
  for (std::size_t at = svg.find(">layer "); at != std::string::npos; at = svg.find(">layer ", at + 1)) {
    ++layer_captions;
  }
  if (box_xs.size() != 3 || layer_captions != 2) {
    return fail("svg spreads a cycle's members over sub-columns under one layer caption");
  }
  return 0;
}

int test_scope_and_depth() {
  const auto graph = fixture();
  auto scoped = request_for();
  scoped.scope = "b";
  auto report = cgraph::build_modules_report(graph, scoped);
  if (report.modules.size() != 2 || module_named(report, "b") == nullptr || module_named(report, "a/x") == nullptr ||
      report.edges.size() != 1 || edge_named(report, "b", "a/x") == nullptr) {
    return fail("scope b reports b's dependencies and keeps a/x as a target; a/x -> a/y is out of scope");
  }
  scoped.scope = "a";
  report = cgraph::build_modules_report(graph, scoped);
  if (report.modules.size() != 2 || module_named(report, "b") != nullptr || report.edges.size() != 1 ||
      edge_named(report, "a/x", "a/y") == nullptr) {
    return fail("scope a keeps a/x and a/y and drops b entirely");
  }
  const auto shallow = cgraph::build_modules_report(graph, request_for(1));
  if (shallow.modules.size() != 2 || module_named(shallow, "a") == nullptr || module_named(shallow, "a")->files != 3 ||
      shallow.edges.size() != 1 || edge_named(shallow, "b", "a") == nullptr) {
    return fail("depth 1 merges a/x and a/y into a (3 files) and folds their edge away");
  }
  auto relative = request_for();
  relative.project_root.clear();
  const auto unrooted = cgraph::build_modules_report(graph, relative);
  if (module_named(unrooted, "proj/a") == nullptr) {
    return fail("without a project root the absolute path's own components name the module");
  }
  return 0;
}

// Thirty modules in a chain with strictly increasing edge weights, so which
// rows survive a budget is unambiguous.
cgraph::GraphSnapshot chain_fixture() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  for (int i = 0; i < 30; ++i) {
    const auto path = "pkg/m" + std::to_string(i) + "/f.py";
    graph.nodes.push_back(file_node(path));
    graph.nodes.push_back(symbol(path, "fn"));
  }
  for (int i = 0; i + 1 < 30; ++i) {
    const auto from = "pkg/m" + std::to_string(i) + "/f.py:fn";
    const auto to = "pkg/m" + std::to_string(i + 1) + "/f.py:fn";
    for (int k = 0; k <= i; ++k) {
      graph.edges.push_back(edge(from, to, "CALLS"));
    }
  }
  return graph;
}

int test_budget_shedding() {
  const auto graph = chain_fixture();
  const auto full = cgraph::build_modules_report(graph, request_for());
  if (full.modules.size() != 30 || full.edges.size() != 29) {
    return fail("chain fixture builds 30 modules and 29 edges");
  }
  const auto full_tokens = cgraph::estimate_report_tokens(cgraph::render_modules_report(full, cgraph::ReportFormat::Json));

  // Phase 1: a budget below the full size but above the bare module list sheds
  // edges only, lightest first, and every surviving row is whole.
  auto edges_only = full;
  cgraph::shed_to_budget(edges_only, cgraph::ReportFormat::Json, full_tokens - 200);
  if (edges_only.omitted_edges == 0 || edges_only.omitted_modules != 0 || edges_only.modules.size() != 30) {
    return fail("a moderate budget sheds edges, never modules");
  }
  if (edges_only.edges.size() + edges_only.omitted_edges != 29) {
    return fail("kept + omitted edges = total");
  }
  for (const auto& kept : edges_only.edges) {
    if (kept.calls < edges_only.edges.back().calls) {
      return fail("edges are shed lightest first (the kept set is the heaviest prefix)");
    }
  }
  const auto rendered = cgraph::render_modules_report(edges_only, cgraph::ReportFormat::Json);
  if (cgraph::estimate_report_tokens(rendered) > full_tokens - 200) {
    return fail("the shed report fits its budget");
  }
  const auto payload = nlohmann::json::parse(rendered);
  if (payload["omitted"]["edges"] != edges_only.omitted_edges || payload["totals"]["edges"] != 29) {
    return fail("json reports omitted and totals");
  }
  for (const auto& row : payload["edges"]) {
    if (!row.contains("from") || !row.contains("to") || !row.contains("calls") || !row.contains("imports")) {
      return fail("every surviving edge row is complete (no partial rows)");
    }
  }

  // Phase 2: a budget too small for the module list sheds modules too, keeping
  // the heaviest (the tail of the chain carries the most calls).
  auto tight = full;
  cgraph::shed_to_budget(tight, cgraph::ReportFormat::Mermaid, 120);
  const auto mermaid = cgraph::render_modules_mermaid(tight);
  if (tight.omitted_modules == 0 || tight.modules.size() + tight.omitted_modules != 30) {
    return fail("a tight budget sheds modules; kept + omitted = total");
  }
  // Weight is in + out: m28 carries 28 + 29 calls, the heaviest; m0 carries 1.
  if (module_named(tight, "pkg/m28") == nullptr || module_named(tight, "pkg/m0") != nullptr) {
    return fail("modules are shed lightest first; kept " + std::to_string(tight.modules.size()) + ":\n" +
                cgraph::render_modules_mermaid(tight));
  }
  if (module_named(tight, "pkg/m28")->layer != 28 || tight.layers.size() != 30 || tight.layers[28] != std::vector<std::string>{"pkg/m28"} ||
      !tight.layers[0].empty()) {
    return fail("a kept module keeps its true layer; layers stay positional with shed layers empty");
  }
  if (mermaid.find("layer 28") == std::string::npos || mermaid.find("subgraph layer0[") != std::string::npos) {
    return fail("mermaid labels layers by their true index and skips shed ones");
  }
  if (cgraph::estimate_report_tokens(mermaid) > 120 || mermaid.find("omitted: ") == std::string::npos) {
    return fail("the mermaid fits the budget and says what was omitted");
  }
  for (const auto& kept : tight.edges) {
    if (module_named(tight, kept.from) == nullptr || module_named(tight, kept.to) == nullptr) {
      return fail("an edge leaves with either shed endpoint");
    }
  }
  for (const auto& layer : tight.layers) {
    for (const auto& name : layer) {
      if (module_named(tight, name) == nullptr) {
        return fail("layers list only kept modules");
      }
    }
  }

  auto unlimited = full;
  cgraph::shed_to_budget(unlimited, cgraph::ReportFormat::Svg, 0);
  if (unlimited.omitted_edges != 0 || unlimited.omitted_modules != 0) {
    return fail("budget 0 disables shedding");
  }
  return 0;
}

int test_renderers() {
  const auto graph = fixture();
  auto request = request_for();
  request.scope = "";
  const auto report = cgraph::build_modules_report(graph, request);
  const auto mermaid = cgraph::render_modules_mermaid(report);
  for (const char* needle : {"graph LR", "subgraph layer0[\"layer 0\"]", "m_a_x[\"a/x<br/>2 files\"]",
                             "m_a_x -->|\"2 calls, 1 import\"| m_a_y", "m_b -->|\"1 call, 1 import\"| m_a_x",
                             "%% cycles: none", "%% omitted: 0 modules, 0 edges (of 3 modules, 2 edges)"}) {
    if (mermaid.find(needle) == std::string::npos) {
      return fail(std::string("mermaid lacks: ") + needle + "\n" + mermaid);
    }
  }
  if (cgraph::render_modules_mermaid(cgraph::build_modules_report(graph, request)) != mermaid) {
    return fail("rendering is deterministic");
  }
  const auto svg = cgraph::render_modules_svg(report);
  for (const char* needle : {"<svg", "layer 0", ">a/x<", ">2 files · 2 symbols<", ">2 calls, 1 import<", "marker-end",
                             "cycles: none", "omitted: 0 modules, 0 edges"}) {
    if (svg.find(needle) == std::string::npos) {
      return fail(std::string("svg lacks: ") + needle);
    }
  }
  if (svg.find("<rect") == std::string::npos || svg.find("<path d=\"M") == std::string::npos) {
    return fail("svg draws boxes and edge paths");
  }
  const auto markdown = cgraph::render_modules_markdown(report);
  for (const char* needle : {"# Module dependencies", "- layer 0: `b`", "| a/x | a/y | 2 | 1 |", "## Cycles\n\nnone",
                             "omitted: 0 modules, 0 edges"}) {
    if (markdown.find(needle) == std::string::npos) {
      return fail(std::string("markdown lacks: ") + needle + "\n" + markdown);
    }
  }
  const auto json = cgraph::modules_report_json(report);
  if (json["view"] != "modules" || json["depth"] != 2 || json["modules"].size() != 3 || json["edges"][0]["from"] != "a/x" ||
      json["layers"].size() != 3 || !json["cycles"].empty() || json["omitted"]["edges"] != 0) {
    return fail("json payload shape");
  }
  return 0;
}

int test_daemon_envelope() {
  cgraph::DaemonState state;
  state.project_root = "/proj";
  cgraph::publish_graph_snapshot(state, fixture());

  const auto json = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "modules"}}));
  if (!json.value("ok", false) || json["result"]["format"] != "json" || json["result"]["modules"].size() != 3 ||
      json["result"]["budget"] != cgraph::kDefaultReportBudget || !json["result"].contains("estimated_tokens") ||
      !json["result"].contains("freshness") || json["result"].contains("graph_state")) {
    return fail("report defaults to the modules view in json under the default budget, decorated like other reads");
  }
  const auto mermaid = cgraph::handle_daemon_request(
      state, cgraph::make_request("report", {{"view", "modules"}, {"format", "mermaid"}, {"scope", "b/"}, {"budget", 0}}));
  if (!mermaid.value("ok", false) || mermaid["result"]["rendered"].get<std::string>().find("graph LR") != 0 ||
      mermaid["result"]["scope"] != "b" || mermaid["result"]["totals"]["modules"] != 2) {
    return fail("mermaid format returns the diagram in `rendered`; a trailing slash on scope is dropped");
  }
  // Every view is implemented; a design request answers with entry points.
  const auto design = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "design"}}));
  if (!design.value("ok", false) || design["result"]["view"] != "design" || !design["result"].contains("entry_points")) {
    return fail("the design view is implemented and answers through the same envelope");
  }
  const auto unknown = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "blueprints"}}));
  if (unknown.value("ok", true) || unknown.value("error", std::string{}).find("unknown report view") == std::string::npos) {
    return fail("an unknown view is an error, not a silent modules report");
  }
  const auto mistyped = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"depth", "two"}}));
  if (mistyped.value("ok", true) || mistyped.value("error", std::string{}).find("invalid request parameter") == std::string::npos) {
    return fail("a mistyped parameter yields an error frame");
  }
  const auto zero_depth = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"depth", 0}}));
  if (zero_depth.value("ok", true)) {
    return fail("depth 0 is rejected");
  }

  // An older graphd never learned the op; the client must say "upgrade", not
  // print an empty report.
  const auto old_daemon = nlohmann::json{{"ok", false}, {"error", "unknown op: report"}};
  const auto hint = cgraph::report_upgrade_hint(old_daemon);
  if (!hint || hint->find("upgrade the daemon") == std::string::npos) {
    return fail("unknown op: report maps to the upgrade hint");
  }
  if (cgraph::report_upgrade_hint(nlohmann::json{{"ok", false}, {"error", "depth must be >= 1"}}) ||
      cgraph::report_upgrade_hint(json)) {
    return fail("other errors and successes carry no upgrade hint");
  }
  if (!cgraph::daemon_op_from_string("report") || cgraph::daemon_op_from_string("report") != cgraph::DaemonOp::Report ||
      std::string{cgraph::daemon_op_name(cgraph::DaemonOp::Report)} != "report") {
    return fail("report is a named daemon op");
  }
  return 0;
}

// ---- types view -----------------------------------------------------------------

cgraph::Node type_node(const std::string& path, const std::string& name, const char* kind, std::uint32_t line) {
  return cgraph::Node{.id = path + ":" + name, .label = name, .source_file = "/proj/" + path,
                      .source_location = cgraph::SourceLocation{.start_line = line, .start_column = 0, .end_line = line + 3, .end_column = 1},
                      .kind = kind};
}

// Adds `owner`'s field nodes and the `defines` edges to them.
void add_members(cgraph::GraphSnapshot& graph, const std::string& path, const std::string& owner,
                 std::initializer_list<const char*> members) {
  for (const char* member : members) {
    const auto id = path + ":" + owner + "::" + member;
    graph.nodes.push_back(cgraph::Node{.id = id, .label = member, .source_file = "/proj/" + path, .kind = "field"});
    graph.edges.push_back(edge(path + ":" + owner, id, "defines"));
  }
}

// Two FileState structs in different headers (the CGraph case), an identical
// pair under different names, a subset pair, a near-miss pair, a pair below the
// member floor, one used and several unused types, and a test-root copy.
cgraph::GraphSnapshot types_fixture() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  for (const char* path : {"src/watch.hpp", "src/drop.hpp", "src/geo.ts", "src/models.ts", "tests/fake.ts", "docs/x.ts"}) {
    graph.nodes.push_back(file_node(path));
  }
  graph.nodes.push_back(type_node("src/watch.hpp", "FileState", "class", 48));
  add_members(graph, "src/watch.hpp", "FileState", {"size", "modified_at", "kind", "token"});
  graph.nodes.push_back(type_node("src/drop.hpp", "FileState", "class", 35));
  add_members(graph, "src/drop.hpp", "FileState", {"size", "modified_at", "drop"});
  graph.nodes.push_back(type_node("src/geo.ts", "Point", "type", 1));
  add_members(graph, "src/geo.ts", "Point", {"x", "y", "z"});
  graph.nodes.push_back(type_node("src/geo.ts", "Vec3", "type", 5));
  add_members(graph, "src/geo.ts", "Vec3", {"z", "y", "x"});
  graph.nodes.push_back(type_node("src/models.ts", "Base", "type", 1));
  add_members(graph, "src/models.ts", "Base", {"id", "name", "createdAt"});
  graph.nodes.push_back(type_node("src/models.ts", "Extended", "type", 6));
  add_members(graph, "src/models.ts", "Extended", {"id", "name", "createdAt", "deletedAt"});
  graph.nodes.push_back(type_node("src/models.ts", "Near", "type", 12));
  add_members(graph, "src/models.ts", "Near", {"a", "b", "c", "d", "e"});
  graph.nodes.push_back(type_node("src/models.ts", "Nearby", "type", 18));
  add_members(graph, "src/models.ts", "Nearby", {"a", "b", "c", "d", "f"});
  graph.nodes.push_back(type_node("src/models.ts", "Tiny", "type", 24));
  add_members(graph, "src/models.ts", "Tiny", {"id"});
  graph.nodes.push_back(type_node("src/models.ts", "Tiny2", "type", 26));
  add_members(graph, "src/models.ts", "Tiny2", {"id"});
  graph.nodes.push_back(type_node("src/models.ts", "Orphan", "class", 30));
  graph.nodes.push_back(type_node("src/models.ts", "Widget", "class", 40));
  graph.nodes.push_back(type_node("tests/fake.ts", "Point", "type", 1));
  add_members(graph, "tests/fake.ts", "Point", {"x", "y", "z"});
  graph.nodes.push_back(type_node("docs/x.ts", "DocOnly", "type", 1));
  // Uses: Point is referenced by a function, Widget is constructed, Extended is
  // inherited from by Widget. Base's only incoming edges are structural
  // (contains from its file) or from its own field, so it is unused despite them.
  graph.nodes.push_back(symbol("src/geo.ts", "draw"));
  graph.edges.push_back(edge("src/geo.ts:draw", "src/geo.ts:Point", "references"));
  graph.edges.push_back(edge("src/geo.ts:draw", "src/models.ts:Widget", "CALLS"));
  graph.edges.push_back(edge("src/models.ts:Widget", "src/models.ts:Extended", "inherits"));
  graph.edges.push_back(edge("file:src/models.ts", "src/models.ts:Base", "contains"));
  graph.edges.push_back(edge("src/models.ts:Base::id", "src/models.ts:Base", "references"));
  // A type that only its own file contains is still unused.
  graph.edges.push_back(edge("file:src/models.ts", "src/models.ts:Orphan", "contains"));
  // Enrichment prose about a type is never a type.
  graph.nodes.push_back(cgraph::Node{.id = "doc:types", .label = "Point", .source_file = "/proj/README.md", .kind = "type"});
  return graph;
}

const cgraph::TypeOverlap* find_overlap(const cgraph::TypesReport& report, const std::string& a, const std::string& b) {
  for (const auto& overlap : report.overlaps) {
    if ((overlap.a.label == a && overlap.b.label == b) || (overlap.a.label == b && overlap.b.label == a)) {
      return &overlap;
    }
  }
  return nullptr;
}

const cgraph::IdenticalTypes* find_identical(const cgraph::TypesReport& report, const std::string& label) {
  for (const auto& group : report.identical) {
    if (std::any_of(group.types.begin(), group.types.end(), [&](const cgraph::TypeRef& t) { return t.label == label; })) {
      return &group;
    }
  }
  return nullptr;
}

bool lists_unreferenced(const cgraph::TypesReport& report, const std::string& label) {
  return std::any_of(report.unreferenced.begin(), report.unreferenced.end(),
                     [&](const cgraph::TypeRef& t) { return t.label == label; });
}

// A contract `schema` (OpenAPI component, proto message, GraphQL type) is a type
// owner too, so a hand-written TypeScript mirror of an API schema shows up: the
// same name in two files as a duplicate, a renamed copy as an identical shape.
int test_types_view_schemas() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  for (const char* path : {"src/openapi.json", "src/types.ts"}) {
    graph.nodes.push_back(file_node(path));
  }
  graph.nodes.push_back(type_node("src/openapi.json", "Notebook", "schema", 40));
  add_members(graph, "src/openapi.json", "Notebook", {"id", "title", "owner"});
  graph.nodes.push_back(type_node("src/types.ts", "Notebook", "type", 3));
  add_members(graph, "src/types.ts", "Notebook", {"id", "title", "owner"});
  graph.nodes.push_back(type_node("src/openapi.json", "NoteDto", "schema", 60));
  add_members(graph, "src/openapi.json", "NoteDto", {"body", "createdAt", "authorId"});
  graph.nodes.push_back(type_node("src/types.ts", "Note", "type", 12));
  add_members(graph, "src/types.ts", "Note", {"authorId", "body", "createdAt"});
  cgraph::ReportRequest request;
  request.view = cgraph::ReportView::Types;
  request.project_root = "/proj";
  const auto report = cgraph::build_types_report(graph, request);
  if (report.total_types != 4 || report.total_with_members != 4) {
    return fail("schema owners count as types: " + std::to_string(report.total_types));
  }
  if (report.duplicates.size() != 1 || report.duplicates[0].label != "Notebook" || report.duplicates[0].declarations.size() != 2 ||
      std::abs(report.duplicates[0].min_jaccard - 1.0) > 1e-9) {
    return fail("an API schema and its TypeScript mirror are one duplicate row");
  }
  const auto* group = find_identical(report, "NoteDto");
  if (group == nullptr || group->types.size() != 2) {
    return fail("a renamed mirror of a schema is an identical shape");
  }
  return 0;
}

int test_types_view() {
  cgraph::ReportRequest request;
  request.view = cgraph::ReportView::Types;
  request.project_root = "/proj";
  const auto report = cgraph::build_types_report(types_fixture(), request);

  // 13 type nodes: the test-root copy is out, docs/ is in (no scope given), the
  // enrichment node never counts.
  if (report.total_types != 13 || report.total_with_members != 10) {
    return fail("types: total_types counts class/type nodes outside test roots; with_members those with fields (" +
                std::to_string(report.total_types) + ", " + std::to_string(report.total_with_members) + ")");
  }
  // Duplicates: FileState in two headers, 2 of 5 members shared; paths root-relative.
  if (report.duplicates.size() != 1 || report.duplicates[0].label != "FileState" ||
      report.duplicates[0].declarations.size() != 2 || report.duplicates[0].declarations[0].source_file != "src/drop.hpp" ||
      report.duplicates[0].declarations[0].line != 35 ||
      report.duplicates[0].declarations[1].members != std::vector<std::string>{"kind", "modified_at", "size", "token"} ||
      std::abs(report.duplicates[0].min_jaccard - 0.4) > 1e-9 || std::abs(report.duplicates[0].max_jaccard - 0.4) > 1e-9) {
    return fail("types: FileState declared in two files is one duplicate row with member overlap 0.40");
  }
  // Identical: Point and Vec3 form one group (declaration order irrelevant).
  const auto* group = find_identical(report, "Point");
  if (report.identical.size() != 1 || group == nullptr || group->types.size() != 2 || group->types[0].label != "Point" ||
      group->types[1].label != "Vec3" || group->shape != std::vector<std::string>{"x", "y", "z"}) {
    return fail("types: Point and Vec3 are one identical group with the shared shape");
  }
  // Overlaps: Base ⊂ Extended (3 of 4, Jaccard 0.75, at least half) is a subset
  // row below the threshold; Near/Nearby (0.67) and Tiny/Tiny2 (member floor) are not rows.
  if (report.overlaps.size() != 1) {
    return fail("types: exactly one overlap row (" + std::to_string(report.overlaps.size()) + ")");
  }
  const auto* subset = find_overlap(report, "Base", "Extended");
  if (subset == nullptr || subset->relation != "subset" || subset->a.label != "Base" || subset->shared != 3 ||
      std::abs(subset->jaccard - 0.75) > 1e-9) {
    return fail("types: Base is a subset of Extended, smaller type first");
  }
  if (find_overlap(report, "Near", "Nearby") != nullptr || find_overlap(report, "Tiny", "Tiny2") != nullptr ||
      find_identical(report, "Tiny") != nullptr) {
    return fail("types: a 0.67 overlap under threshold 0.80 and a pair under the member floor are not rows");
  }
  // Unreferenced: Base (structural and own-field edges only), Vec3, Near, Nearby,
  // Tiny, Tiny2, Orphan, DocOnly and both FileStates; not Point (referenced),
  // Widget (constructed) or Extended (inherited from). Most members first.
  for (const char* label : {"Base", "Vec3", "Orphan", "DocOnly", "FileState"}) {
    if (!lists_unreferenced(report, label)) {
      return fail(std::string("types: unreferenced lists ") + label);
    }
  }
  for (const char* label : {"Point", "Widget", "Extended"}) {
    if (lists_unreferenced(report, label)) {
      return fail(std::string("types: a referenced, constructed or inherited type is not unreferenced: ") + label);
    }
  }
  if (report.unreferenced.size() != 10 || report.unreferenced[0].label != "Near" || report.unreferenced[0].members.size() != 5) {
    return fail("types: ten unreferenced types, the widest first (" + std::to_string(report.unreferenced.size()) + ")");
  }

  // Lower thresholds and floors widen the report; scope narrows it.
  cgraph::ReportRequest loose = request;
  loose.threshold = 0.6;
  loose.min_members = 1;
  const auto wide = cgraph::build_types_report(types_fixture(), loose);
  const auto* near = find_overlap(wide, "Near", "Nearby");
  const auto* tiny = find_identical(wide, "Tiny");
  if (near == nullptr || near->relation != "overlap" || near->shared != 4 || tiny == nullptr || tiny->types.size() != 2 ||
      wide.identical.size() != 2 || wide.identical[0].shape.size() != 3) {
    return fail("types: threshold 0.6 admits Near/Nearby as overlap; min_members 1 admits Tiny/Tiny2 as a second group, widest first");
  }
  // Tiny {id} sits inside Base {id, name, createdAt}, but at a third of it: not a subset row.
  if (find_overlap(wide, "Tiny", "Base") != nullptr || wide.overlaps.size() != 2) {
    return fail("types: a contained type smaller than half the larger one is not a subset row");
  }
  cgraph::ReportRequest scoped = request;
  scoped.scope = "src";
  const auto src_only = cgraph::build_types_report(types_fixture(), scoped);
  if (src_only.total_types != 12 || lists_unreferenced(src_only, "DocOnly")) {
    return fail("types: scope keeps only types whose file is under the prefix");
  }
  cgraph::ReportRequest with_tests = request;
  with_tests.include_tests = true;
  const auto tests_too = cgraph::build_types_report(types_fixture(), with_tests);
  if (tests_too.total_types != 14 || tests_too.duplicates.size() != 2 || tests_too.duplicates[0].label != "Point" ||
      tests_too.duplicates[0].min_jaccard != 1.0 || tests_too.duplicates[1].label != "FileState" ||
      find_identical(tests_too, "Vec3") == nullptr || find_identical(tests_too, "Vec3")->types.size() != 3) {
    return fail("types: include_tests admits the test-root Point: a copy (1.0) that outranks FileState, and a third identical member");
  }
  return 0;
}

int test_types_budget_and_renderers() {
  cgraph::ReportRequest request;
  request.view = cgraph::ReportView::Types;
  request.project_root = "/proj";
  const auto full = cgraph::build_types_report(types_fixture(), request);

  // Shedding drops whole rows from the last section first: unreferenced, then
  // overlaps, then duplicates, then identical groups.
  auto shed = full;
  const auto full_tokens = cgraph::estimate_report_tokens(cgraph::render_types_report(full, cgraph::ReportFormat::Json));
  cgraph::shed_to_budget(shed, cgraph::ReportFormat::Json, full_tokens - 1);
  if (shed.unreferenced.size() >= full.unreferenced.size() || shed.overlaps.size() != 1 || shed.duplicates.size() != 1 ||
      shed.identical.size() != 1 || shed.omitted_unreferenced != full.unreferenced.size() - shed.unreferenced.size() ||
      shed.omitted_overlaps != 0 || shed.total_unreferenced != full.total_unreferenced) {
    return fail("types: one token short sheds the tail of unreferenced first and reports it as omitted");
  }
  auto minimal = full;
  cgraph::shed_to_budget(minimal, cgraph::ReportFormat::Markdown, 150);
  if (!minimal.unreferenced.empty() || !minimal.overlaps.empty() || minimal.omitted_overlaps != 1 ||
      minimal.omitted_unreferenced != full.unreferenced.size() || minimal.identical.size() != 1 ||
      cgraph::estimate_report_tokens(cgraph::render_types_markdown(minimal)) > 150) {
    return fail("types: a tight budget keeps the identical group before anything else and fits");
  }
  auto unlimited = full;
  cgraph::shed_to_budget(unlimited, cgraph::ReportFormat::Json, 0);
  if (unlimited.unreferenced.size() != full.unreferenced.size() || unlimited.omitted_unreferenced != 0) {
    return fail("types: budget 0 sheds nothing");
  }

  const auto json = cgraph::types_report_json(full);
  if (json["view"] != "types" || json["totals"]["types"] != 13 || json["totals"]["identical"] != 1 ||
      json["totals"]["duplicates"] != 1 || json["totals"]["overlaps"] != 1 || json["totals"]["unreferenced"] != 10 ||
      json["omitted"]["unreferenced"] != 0 || json["identical"][0]["shape"].size() != 3 ||
      json["identical"][0]["types"][1]["label"] != "Vec3" || json["identical"][0]["types"][1]["line"] != 5 ||
      json["duplicates"][0]["declarations"][0]["file"] != "src/drop.hpp" ||
      json["duplicates"][0]["declarations"][0]["line"] != 35 || json["overlaps"][0]["relation"] != "subset" ||
      json["unreferenced"][0]["label"] != "Near" || json["threshold"] != 0.8 || json["min_members"] != 3) {
    return fail("types: json carries identical/duplicates/overlaps/unreferenced with file:line and members, plus totals and omitted");
  }
  const auto markdown = cgraph::render_types_markdown(full);
  for (const char* expected : {"# Type definitions", "## Identical shapes", "| x, y, z (3) | `Point` src/geo.ts:1<br>`Vec3` src/geo.ts:5 |",
                               "## Duplicates", "| `FileState` (2 declarations) |", "src/drop.hpp:35", "## Overlapping shapes",
                               "| `Base` src/models.ts:1 (3) | `Extended` src/models.ts:6 (4) | subset | 3 | 0.75 |",
                               "## Unreferenced", "| `Near` | type | src/models.ts:12 | 5 |",
                               "omitted: 0 identical, 0 duplicates, 0 overlaps, 0 unreferenced (of 1, 1, 1, 10)"}) {
    if (markdown.find(expected) == std::string::npos) {
      return fail(std::string("types markdown lacks: ") + expected);
    }
  }
  if (cgraph::render_types_report(full, cgraph::ReportFormat::Mermaid) != markdown) {
    return fail("types: the diagram formats render as markdown at the renderer level");
  }
  return 0;
}

int test_types_envelope() {
  cgraph::DaemonState state;
  state.project_root = "/proj";
  cgraph::publish_graph_snapshot(state, types_fixture());

  const auto json = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "types"}}));
  if (!json.value("ok", false) || json["result"]["view"] != "types" || json["result"]["format"] != "json" ||
      json["result"]["identical"].size() != 1 || json["result"]["duplicates"].size() != 1 ||
      json["result"]["overlaps"].size() != 1 || json["result"]["unreferenced"].size() != 10 ||
      json["result"]["budget"] != cgraph::kDefaultReportBudget || !json["result"].contains("estimated_tokens") ||
      !json["result"].contains("freshness")) {
    return fail("types envelope: json result carries the four sections, budget, tokens, freshness");
  }
  const auto markdown = cgraph::handle_daemon_request(
      state, cgraph::make_request("report", {{"view", "types"}, {"format", "markdown"}, {"threshold", 0.6}, {"min_members", 1}}));
  if (!markdown.value("ok", false) || !markdown["result"].contains("rendered") || markdown["result"].contains("unreferenced") ||
      markdown["result"].contains("identical") || markdown["result"]["totals"]["identical"] != 2 ||
      markdown["result"]["totals"]["overlaps"] != 2 || markdown["result"]["threshold"] != 0.6 ||
      markdown["result"]["min_members"] != 1 ||
      markdown["result"]["rendered"].get<std::string>().find("| `Near`") == std::string::npos) {
    return fail("types envelope: markdown returns `rendered` plus totals, and forwards threshold/min_members");
  }
  for (const char* diagram : {"mermaid", "svg"}) {
    const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "types"}, {"format", diagram}}));
    if (response.value("ok", true) || response.value("code", std::string{}) != "report_format_unsupported") {
      return fail(std::string("types envelope: a diagram format is a typed error: ") + diagram);
    }
  }
  const auto bad_threshold = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "types"}, {"threshold", 1.5}}));
  const auto bad_floor = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "types"}, {"min_members", 0}}));
  if (bad_threshold.value("ok", true) || bad_floor.value("ok", true)) {
    return fail("types envelope: threshold outside [0,1] and min_members 0 are rejected");
  }
  return 0;
}

// ---- clones view ----------------------------------------------------------------

// A fingerprint from a list of shingle hashes; tokens default well above the floor.
cgraph::FunctionFingerprint fp(std::initializer_list<std::uint64_t> shingles, std::uint32_t tokens = 60) {
  cgraph::FunctionFingerprint out;
  out.shingles.assign(shingles.begin(), shingles.end());
  std::sort(out.shingles.begin(), out.shingles.end());
  out.tokens = tokens;
  return out;
}

cgraph::Node function_node(const std::string& path, const std::string& name, std::uint32_t line) {
  return cgraph::Node{.id = path + ":" + name, .label = name, .source_file = "/proj/" + path,
                      .source_location = cgraph::SourceLocation{.start_line = line, .start_column = 0, .end_line = line + 5, .end_column = 1},
                      .kind = "function"};
}

// Three production copies (two exact, one at 0.8), a 0.6 near miss, an
// unrelated body, a body under the token floor that would otherwise match, two
// test-root copies, and a function with no fingerprint at all.
cgraph::GraphSnapshot clones_fixture() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  for (const char* path : {"src/a.ts", "src/b.ts", "src/c.ts", "tests/t1.ts", "tests/t2.ts"}) {
    graph.nodes.push_back(file_node(path));
  }
  graph.nodes.push_back(function_node("src/a.ts", "writeFile", 10));
  graph.fingerprints["src/a.ts:writeFile"] = fp({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  graph.nodes.push_back(function_node("src/b.ts", "persist", 20));
  graph.fingerprints["src/b.ts:persist"] = fp({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  graph.nodes.push_back(function_node("src/c.ts", "save", 30));
  graph.fingerprints["src/c.ts:save"] = fp({1, 2, 3, 4, 5, 6, 7, 8, 11, 12});  // 8/12 = 0.67 with the copies... see below
  graph.nodes.push_back(function_node("src/c.ts", "nearMiss", 40));
  graph.fingerprints["src/c.ts:nearMiss"] = fp({1, 2, 3, 4, 5, 6, 21, 22, 23, 24});  // 6/14 = 0.43
  graph.nodes.push_back(function_node("src/c.ts", "unrelated", 50));
  graph.fingerprints["src/c.ts:unrelated"] = fp({31, 32, 33, 34, 35, 36, 37, 38, 39, 40});
  graph.nodes.push_back(function_node("src/c.ts", "shortCopy", 60));
  graph.fingerprints["src/c.ts:shortCopy"] = fp({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 12);  // under min_tokens 30
  graph.nodes.push_back(function_node("src/c.ts", "noFingerprint", 70));
  graph.nodes.push_back(function_node("tests/t1.ts", "helper", 1));
  graph.fingerprints["tests/t1.ts:helper"] = fp({51, 52, 53, 54, 55, 56, 57, 58, 59, 60});
  graph.nodes.push_back(function_node("tests/t2.ts", "helper", 1));
  graph.fingerprints["tests/t2.ts:helper"] = fp({51, 52, 53, 54, 55, 56, 57, 58, 59, 60});
  // Enrichment prose is never a function.
  graph.nodes.push_back(cgraph::Node{.id = "doc:fn", .label = "writeFile", .source_file = "/proj/README.md", .kind = "function"});
  return graph;
}

// Two identical copies whose every shingle but one is boilerplate shared by 600
// other functions. The hot shingles are skipped when gathering candidates, but
// the similarity must still be computed over the full sets: 1.0, not 1/6.
cgraph::GraphSnapshot hot_shingle_fixture() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  graph.nodes.push_back(file_node("src/hot.ts"));
  for (std::uint32_t i = 0; i < 600; ++i) {
    const auto name = "filler" + std::to_string(i);
    graph.nodes.push_back(function_node("src/hot.ts", name, 100 + i * 10));
    graph.fingerprints["src/hot.ts:" + name] = fp({1000, 1001, 1002, 1003, 1004, 5000 + i});
  }
  graph.nodes.push_back(function_node("src/hot.ts", "copyA", 1));
  graph.fingerprints["src/hot.ts:copyA"] = fp({1000, 1001, 1002, 1003, 1004, 77});
  graph.nodes.push_back(function_node("src/hot.ts", "copyB", 10));
  graph.fingerprints["src/hot.ts:copyB"] = fp({1000, 1001, 1002, 1003, 1004, 77});
  return graph;
}

int test_clones_view() {
  cgraph::ReportRequest request;
  request.view = cgraph::ReportView::Clones;
  request.project_root = "/proj";
  auto report = cgraph::build_clones_report(clones_fixture(), request);

  // 9 functions in scope, 8 fingerprinted, 7 at or above the floor.
  if (report.total_functions != 9 || report.total_fingerprinted != 8 || report.total_eligible != 7) {
    return fail("clones: totals count functions, fingerprinted, eligible (" + std::to_string(report.total_functions) + ", " +
                std::to_string(report.total_fingerprinted) + ", " + std::to_string(report.total_eligible) + ")");
  }
  if (report.hint.empty() || report.hint.find("1 of 9") == std::string::npos) {
    return fail("clones: a function without a fingerprint produces the rescan hint");
  }
  // One production class: writeFile + persist (identical). save is 8/12 = 0.67
  // with them, nearMiss lower, shortCopy under the floor, unrelated unrelated.
  if (report.classes.size() != 1 || report.classes[0].members.size() != 2 || report.classes[0].similarity != 1.0 ||
      report.classes[0].tokens != 60 || report.classes[0].members[0].label != "writeFile" ||
      report.classes[0].members[0].source_file != "src/a.ts" || report.classes[0].members[0].line != 10 ||
      report.classes[0].members[0].end_line != 15 || report.classes[0].members[1].label != "persist") {
    return fail("clones: one production class of the two identical copies, members by file, root-relative, with extents");
  }
  // The two test helpers form a test class, bucketed apart.
  if (report.test_classes.size() != 1 || report.test_classes[0].members.size() != 2 ||
      report.test_classes[0].members[0].source_file != "tests/t1.ts" || report.total_classes != 1 ||
      report.total_test_classes != 1 || report.total_members != 4) {
    return fail("clones: test-root copies are a test class, counted in members");
  }
  // Lower the threshold: save joins the copies (0.67 >= 0.6), nearMiss does not
  // (0.43); the class's similarity is its lowest pair.
  cgraph::ReportRequest loose = request;
  loose.threshold = 0.6;
  const auto wide = cgraph::build_clones_report(clones_fixture(), loose);
  if (wide.classes.size() != 1 || wide.classes[0].members.size() != 3 || wide.classes[0].members[2].label != "save" ||
      std::abs(wide.classes[0].similarity - 8.0 / 12.0) > 1e-9) {
    return fail("clones: threshold 0.6 admits the 0.67 copy; class similarity is the lowest pair");
  }
  // Lower the token floor: shortCopy joins.
  cgraph::ReportRequest short_ok = request;
  short_ok.min_tokens = 10;
  const auto with_short = cgraph::build_clones_report(clones_fixture(), short_ok);
  if (with_short.classes.size() != 1 || with_short.classes[0].members.size() != 3 || with_short.classes[0].tokens != 12 ||
      with_short.total_eligible != 8) {
    return fail("clones: min_tokens 10 admits the short copy and the class tokens is the shortest member");
  }
  // include_tests merges the test class into classes; scope narrows candidates.
  cgraph::ReportRequest merged = request;
  merged.include_tests = true;
  const auto all = cgraph::build_clones_report(clones_fixture(), merged);
  if (all.classes.size() != 2 || !all.test_classes.empty() || all.total_test_classes != 0) {
    return fail("clones: include_tests merges test classes into classes");
  }
  cgraph::ReportRequest scoped = request;
  scoped.scope = "tests";
  const auto tests_only = cgraph::build_clones_report(clones_fixture(), scoped);
  if (tests_only.total_functions != 2 || !tests_only.classes.empty() || tests_only.test_classes.size() != 1) {
    return fail("clones: scope keeps only functions under the prefix");
  }

  // Hot shingles are skipped for candidacy only; the pair's Jaccard is exact.
  const auto hot = cgraph::build_clones_report(hot_shingle_fixture(), request);
  if (hot.classes.size() != 1 || hot.classes[0].members.size() != 2 || hot.classes[0].similarity != 1.0 ||
      hot.classes[0].members[0].label != "copyA") {
    return fail("clones: two copies whose shared shingles are mostly boilerplate still score 1.0 (" +
                std::to_string(hot.classes.size()) + " classes)");
  }

  // Shedding: test classes go first, then production classes.
  const auto full_tokens = cgraph::estimate_report_tokens(cgraph::render_clones_report(report, cgraph::ReportFormat::Json));
  auto shed = report;
  cgraph::shed_to_budget(shed, cgraph::ReportFormat::Json, full_tokens - 1);
  if (shed.classes.size() != 1 || !shed.test_classes.empty() || shed.omitted_test_classes != 1 || shed.omitted_classes != 0) {
    return fail("clones: one token short sheds the test class first");
  }
  auto unlimited = report;
  cgraph::shed_to_budget(unlimited, cgraph::ReportFormat::Markdown, 0);
  if (unlimited.test_classes.size() != 1) {
    return fail("clones: budget 0 sheds nothing");
  }

  const auto json = cgraph::clones_report_json(report);
  if (json["view"] != "clones" || json["totals"]["classes"] != 1 || json["totals"]["test_classes"] != 1 ||
      json["totals"]["fingerprinted"] != 8 || json["classes"][0]["size"] != 2 || json["classes"][0]["similarity"] != 1.0 ||
      json["classes"][0]["members"][0]["file"] != "src/a.ts" || json["classes"][0]["members"][0]["line"] != 10 ||
      json["classes"][0]["members"][0]["end_line"] != 15 || json["classes"][0]["members"][0]["tokens"] != 60 ||
      !json.contains("hint") || json["min_tokens"] != 30) {
    return fail("clones: json carries classes, test_classes, totals, omitted, min_tokens and the hint");
  }
  const auto markdown = cgraph::render_clones_markdown(report);
  for (const char* expected : {"# Function clones", "## Clone classes", "| 2 | 1.00 | 60 | `writeFile` src/a.ts:10-15<br>`persist` src/b.ts:20-25 |",
                               "## Test-only clone classes", "`helper` tests/t1.ts:1-6", "> 1 of 9 functions have no fingerprint",
                               "omitted: 0 classes, 0 test classes (of 1 class, 1 test class)"}) {
    if (markdown.find(expected) == std::string::npos) {
      return fail(std::string("clones markdown lacks: ") + expected);
    }
  }
  return 0;
}

int test_clones_envelope() {
  cgraph::DaemonState state;
  state.project_root = "/proj";
  cgraph::publish_graph_snapshot(state, clones_fixture());
  const auto json = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "clones"}}));
  if (!json.value("ok", false) || json["result"]["view"] != "clones" || json["result"]["classes"].size() != 1 ||
      json["result"]["test_classes"].size() != 1 || !json["result"].contains("hint") ||
      json["result"]["budget"] != cgraph::kDefaultReportBudget) {
    return fail("clones envelope: json result carries both buckets, the hint and the budget");
  }
  const auto markdown = cgraph::handle_daemon_request(
      state, cgraph::make_request("report", {{"view", "clones"}, {"format", "markdown"}, {"threshold", 0.6}, {"min_tokens", 10}}));
  if (!markdown.value("ok", false) || !markdown["result"].contains("rendered") || markdown["result"].contains("classes") ||
      markdown["result"]["totals"]["members"] != 6 || markdown["result"]["threshold"] != 0.6 || markdown["result"]["min_tokens"] != 10) {
    return fail("clones envelope: markdown returns `rendered` plus totals and forwards threshold/min_tokens");
  }
  for (const char* diagram : {"mermaid", "svg"}) {
    const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "clones"}, {"format", diagram}}));
    if (response.value("ok", true) || response.value("code", std::string{}) != "report_format_unsupported") {
      return fail(std::string("clones envelope: a diagram format is a typed error: ") + diagram);
    }
  }
  const auto bad_floor = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "clones"}, {"min_tokens", -1}}));
  if (bad_floor.value("ok", true)) {
    return fail("clones envelope: a negative min_tokens is rejected");
  }
  return 0;
}

// ---- design view ----------------------------------------------------------------

void add_call(cgraph::GraphSnapshot& graph, const std::string& from, const std::string& to) {
  graph.edges.push_back(edge(from, to, "CALLS"));
}

// main with seven callees (branch cap), a route handler, a Next.js page, an
// uncalled root, a leaf nobody calls, a two-function cycle no entry reaches, a
// test-root caller, and enrichment prose.
cgraph::GraphSnapshot design_fixture() {
  cgraph::GraphSnapshot graph;
  graph.build_state = cgraph::BuildState::DeterministicReady;
  for (const char* path : {"src/cli/main.cpp", "src/engine/run.cpp", "src/api.ts", "app/dashboard/page.tsx", "src/lib.ts", "tests/t.ts"}) {
    graph.nodes.push_back(file_node(path));
  }
  graph.nodes.push_back(function_node("src/cli/main.cpp", "main", 5));
  graph.nodes.push_back(function_node("src/engine/run.cpp", "run", 10));
  graph.nodes.push_back(function_node("src/engine/run.cpp", "store", 20));
  graph.nodes.push_back(function_node("src/engine/run.cpp", "persist", 30));
  graph.nodes.push_back(function_node("src/engine/run.cpp", "parse", 40));
  graph.nodes.push_back(function_node("src/engine/run.cpp", "lex", 50));
  for (const char* h : {"h1", "h2", "h3", "h4", "h5"}) {
    graph.nodes.push_back(function_node("src/engine/run.cpp", h, 60));
    add_call(graph, "src/cli/main.cpp:main", std::string("src/engine/run.cpp:") + h);
  }
  add_call(graph, "src/cli/main.cpp:main", "src/engine/run.cpp:run");
  add_call(graph, "src/cli/main.cpp:main", "src/engine/run.cpp:parse");
  add_call(graph, "src/engine/run.cpp:run", "src/engine/run.cpp:store");
  add_call(graph, "src/engine/run.cpp:store", "src/engine/run.cpp:persist");
  add_call(graph, "src/engine/run.cpp:parse", "src/engine/run.cpp:lex");
  graph.nodes.push_back(function_node("src/api.ts", "app.get /health", 3));
  graph.nodes.push_back(function_node("src/api.ts", "ping", 10));
  graph.nodes.push_back(function_node("src/api.ts", "fmt", 20));
  add_call(graph, "src/api.ts:app.get /health", "src/api.ts:ping");
  add_call(graph, "src/api.ts:ping", "src/api.ts:fmt");
  graph.nodes.push_back(function_node("app/dashboard/page.tsx", "Page", 1));
  graph.nodes.push_back(function_node("app/dashboard/page.tsx", "load", 12));
  graph.nodes.push_back(function_node("app/dashboard/page.tsx", "fetchAll", 20));
  add_call(graph, "app/dashboard/page.tsx:Page", "app/dashboard/page.tsx:load");
  add_call(graph, "app/dashboard/page.tsx:load", "app/dashboard/page.tsx:fetchAll");
  add_call(graph, "app/dashboard/page.tsx:fetchAll", "src/api.ts:fmt");
  graph.nodes.push_back(function_node("src/lib.ts", "exported", 1));
  graph.nodes.push_back(function_node("src/lib.ts", "helper", 8));
  graph.nodes.push_back(function_node("src/lib.ts", "leaf", 15));
  graph.nodes.push_back(function_node("src/lib.ts", "cycA", 20));
  graph.nodes.push_back(function_node("src/lib.ts", "cycB", 30));
  add_call(graph, "src/lib.ts:exported", "src/lib.ts:helper");
  add_call(graph, "src/lib.ts:cycA", "src/lib.ts:cycB");
  add_call(graph, "src/lib.ts:cycB", "src/lib.ts:cycA");
  graph.nodes.push_back(function_node("tests/t.ts", "test_main", 1));
  add_call(graph, "tests/t.ts:test_main", "src/engine/run.cpp:run");
  graph.nodes.push_back(cgraph::Node{.id = "doc:main", .label = "main", .source_file = "/proj/README.md", .kind = "function"});
  return graph;
}

const cgraph::DesignEntry* find_entry(const cgraph::DesignReport& report, const std::string& label) {
  for (const auto& entry : report.entries) {
    if (entry.label == label) {
      return &entry;
    }
  }
  return nullptr;
}

int test_design_view() {
  cgraph::ReportRequest request;
  request.view = cgraph::ReportView::Design;
  request.project_root = "/proj";
  const auto report = cgraph::build_design_report(design_fixture(), request);

  // 22 functions outside test roots; four entry points of four kinds, by reach.
  if (report.total_functions != 22 || report.total_entries != 4 || report.entries.size() != 4 ||
      report.entries[0].label != "main" || report.entries[1].label != "Page" || report.entries[2].label != "app.get /health" ||
      report.entries[3].label != "exported") {
    return fail("design: four entry points ranked by reach (main 10, Page 3, route 2, exported 1)");
  }
  const auto* main = find_entry(report, "main");
  const auto* page = find_entry(report, "Page");
  const auto* route = find_entry(report, "app.get /health");
  const auto* exported = find_entry(report, "exported");
  if (main->kind != "main" || page->kind != "page" || route->kind != "route" || exported->kind != "root" ||
      report.by_kind.at("main") != 1 || report.by_kind.at("page") != 1 || report.by_kind.at("route") != 1 ||
      report.by_kind.at("root") != 1) {
    return fail("design: entry kinds are main, page, route, root");
  }
  if (main->reach != 10 || main->fan_out != 7 || main->module != "src/cli" || main->source_file != "src/cli/main.cpp" ||
      main->line != 5 || page->reach != 3 || route->reach != 2 || exported->reach != 1) {
    return fail("design: reach counts every function transitively called; module and file are root-relative");
  }
  // Not entries: leaf (no callees), cycA/cycB (each called by the other),
  // helper/run/store (called), test_main (test root).
  for (const char* label : {"leaf", "cycA", "cycB", "helper", "run", "test_main"}) {
    if (find_entry(report, label) != nullptr) {
      return fail(std::string("design: not an entry point: ") + label);
    }
  }
  // main's flow: children by reach (run 2, parse 1, then h1, h2 alphabetically),
  // three more counted; run -> store -> persist to the third hop.
  const auto& flow = main->flow;
  if (flow.children.size() != 4 || flow.more != 3 || flow.children[0].label != "run" || flow.children[1].label != "parse" ||
      flow.children[2].label != "h1" || flow.children[3].label != "h2" || flow.children[0].children.size() != 1 ||
      flow.children[0].children[0].label != "store" || flow.children[0].children[0].children.size() != 1 ||
      flow.children[0].children[0].children[0].label != "persist" || !flow.children[0].children[0].children[0].children.empty()) {
    return fail("design: the flow draws four children by reach with `more`, three hops deep");
  }
  // Layers: 0 = the four entries; 1 = run, parse, h1..h5, ping, load, helper (10);
  // 2 = store, lex, fmt, fetchAll (4); 3 = persist (1). fmt is reached at depth 2
  // via ping, not 3 via fetchAll.
  if (report.layers.size() != 4 || report.layers[0].functions != 4 || report.layers[1].functions != 10 ||
      report.layers[2].functions != 4 || report.layers[3].functions != 1 || report.layers[1].modules.front() != "src/engine") {
    return fail("design: layers count functions by shortest call distance, with the modules that hold them");
  }
  if (report.total_reached != 15 || report.total_unreached != 3 || report.unreached_samples.size() != 3 ||
      report.unreached_samples[0] != "cycA") {
    return fail("design: 15 reached, 3 unreached (leaf, cycA, cycB), most called first");
  }

  // hops 1 flattens the flows; include_tests admits the test caller as a root;
  // scope narrows the candidate set.
  cgraph::ReportRequest shallow = request;
  shallow.hops = 1;
  const auto one_hop = cgraph::build_design_report(design_fixture(), shallow);
  if (find_entry(one_hop, "main")->flow.children[0].children.size() != 0 || find_entry(one_hop, "main")->reach != 10) {
    return fail("design: hops bounds the drawn flow, not the reach");
  }
  cgraph::ReportRequest with_tests = request;
  with_tests.include_tests = true;
  const auto tests_too = cgraph::build_design_report(design_fixture(), with_tests);
  const auto* test_main = find_entry(tests_too, "test_main");
  if (tests_too.total_functions != 23 || test_main == nullptr || test_main->kind != "root" || test_main->reach != 3) {
    return fail("design: include_tests admits the test caller as a root entry");
  }
  cgraph::ReportRequest scoped = request;
  scoped.scope = "src/api.ts";
  scoped.scope = "src";
  const auto src_only = cgraph::build_design_report(design_fixture(), scoped);
  if (src_only.total_functions != 19 || find_entry(src_only, "Page") != nullptr || find_entry(src_only, "main") == nullptr) {
    return fail("design: scope keeps only functions whose file is under the prefix");
  }

  // Shedding drops whole entry points from the tail of the ranking.
  const auto full_tokens = cgraph::estimate_report_tokens(cgraph::render_design_report(report, cgraph::ReportFormat::Json));
  auto shed = report;
  cgraph::shed_to_budget(shed, cgraph::ReportFormat::Json, full_tokens - 1);
  if (shed.entries.size() != 3 || shed.entries.back().label != "app.get /health" || shed.omitted_entries != 1 ||
      shed.total_entries != 4 || shed.layers.size() != 4) {
    return fail("design: one token short sheds the last entry point and keeps the layers");
  }

  const auto json = cgraph::design_report_json(report);
  if (json["view"] != "design" || json["totals"]["functions"] != 22 || json["totals"]["entry_points"] != 4 ||
      json["totals"]["by_kind"]["route"] != 1 || json["totals"]["reached"] != 15 || json["totals"]["unreached"] != 3 ||
      json["entry_points"][0]["label"] != "main" || json["entry_points"][0]["flow"]["children"].size() != 4 ||
      json["entry_points"][0]["flow"]["more"] != 3 || json["entry_points"][0]["flow"]["children"][0]["children"][0]["label"] != "store" ||
      json["layers"][1]["functions"] != 10 || json["hops"] != 3) {
    return fail("design: json carries entry points with nested flows, layers, totals by kind and omitted");
  }
  const auto markdown = cgraph::render_design_markdown(report);
  for (const char* expected : {"# Program design", "## Entry points", "| main | `main` | src/cli/main.cpp:5 | src/cli | 7 | 10 |",
                               "| route | `app.get /health` | src/api.ts:3 |", "### main: `main` (src/cli/main.cpp:5)",
                               "- `run` src/engine/run.cpp:10 (reach 2)", "  - `store` src/engine/run.cpp:20 (reach 1)",
                               "    - `persist` src/engine/run.cpp:30", "- +3 more callees", "## Layers", "| 1 | 10 | `src/engine`",
                               "3 unreached from any entry point (most called first): `cycA`, `cycB`, `leaf`",
                               "omitted: 0 of 4 entry points"}) {
    if (markdown.find(expected) == std::string::npos) {
      return fail(std::string("design markdown lacks: ") + expected);
    }
  }
  const auto mermaid = cgraph::render_design_mermaid(report);
  for (const char* expected : {"flowchart TD", "%% main: main (src/cli/main.cpp, reach 10)", "[\"main\"]", "[\"run\"]", " --> ",
                               "([\"+3 more\"])", "class f0 entry", "%% omitted: 0 of 4 entry points"}) {
    if (mermaid.find(expected) == std::string::npos) {
      return fail(std::string("design mermaid lacks: ") + expected);
    }
  }
  // fmt is reached from both the route and the page: one node, two edges.
  if (mermaid.find("[\"fmt\"]") == std::string::npos || mermaid.find("[\"fmt\"]", mermaid.find("[\"fmt\"]") + 1) != std::string::npos) {
    return fail("design mermaid draws a shared callee once");
  }
  return 0;
}

int test_design_envelope() {
  cgraph::DaemonState state;
  state.project_root = "/proj";
  cgraph::publish_graph_snapshot(state, design_fixture());
  const auto json = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "design"}}));
  if (!json.value("ok", false) || json["result"]["view"] != "design" || json["result"]["entry_points"].size() != 4 ||
      json["result"]["layers"].size() != 4 || json["result"]["budget"] != cgraph::kDefaultReportBudget) {
    return fail("design envelope: json result carries entry points, layers and the budget");
  }
  const auto mermaid = cgraph::handle_daemon_request(
      state, cgraph::make_request("report", {{"view", "design"}, {"format", "mermaid"}, {"hops", 1}, {"budget", 0}}));
  if (!mermaid.value("ok", false) || mermaid["result"]["rendered"].get<std::string>().find("flowchart TD") != 0 ||
      mermaid["result"].contains("entry_points") || mermaid["result"]["hops"] != 1 || mermaid["result"]["totals"]["entry_points"] != 4) {
    return fail("design envelope: mermaid returns the flowchart in `rendered` plus totals and forwards hops");
  }
  const auto markdown = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "design"}, {"format", "markdown"}}));
  if (!markdown.value("ok", false) || markdown["result"]["rendered"].get<std::string>().find("# Program design") != 0) {
    return fail("design envelope: markdown returns the report in `rendered`");
  }
  const auto svg = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "design"}, {"format", "svg"}}));
  if (svg.value("ok", true) || svg.value("code", std::string{}) != "report_format_unsupported") {
    return fail("design envelope: svg is a typed error");
  }
  const auto bad_hops = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", "design"}, {"hops", 0}}));
  if (bad_hops.value("ok", true)) {
    return fail("design envelope: hops 0 is rejected");
  }
  return 0;
}

}  // namespace

int main() {
  for (const auto test : {test_grouping_and_test_exclusion, test_layers, test_cycles, test_scope_and_depth,
                          test_budget_shedding, test_renderers, test_daemon_envelope, test_types_view,
                          test_types_view_schemas, test_types_budget_and_renderers, test_types_envelope, test_clones_view,
                          test_clones_envelope,
                          test_design_view, test_design_envelope}) {
    if (const int rc = test(); rc != 0) {
      return rc;
    }
  }
  return 0;
}
