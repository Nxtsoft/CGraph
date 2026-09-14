// report.cpp: the `report` op and its modules view -- grouping by directory
// depth, cross-module edge aggregation, longest-path layering, cycle listing,
// scope and test-root filtering, whole-row budget shedding with `omitted`, the
// four renderers, and the daemon envelope (reserved views, mistyped params,
// the upgrade hint for a graphd that predates the op).
#include "cgraph/report.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/protocol.hpp"

#include <algorithm>
#include <iostream>
#include <string>

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
  return 0;
}

int test_scope_and_depth() {
  const auto graph = fixture();
  auto scoped = request_for();
  scoped.scope = "b/";  // a trailing slash is tolerated by the daemon parser; here it must match exactly
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
  for (const char* reserved : {"design", "clones", "types"}) {
    const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("report", {{"view", reserved}}));
    if (response.value("ok", true) || response.value("code", std::string{}) != "report_view_not_implemented") {
      return fail(std::string("reserved view answers a typed not-implemented error: ") + reserved);
    }
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

}  // namespace

int main() {
  for (const auto test : {test_grouping_and_test_exclusion, test_layers, test_cycles, test_scope_and_depth,
                          test_budget_shedding, test_renderers, test_daemon_envelope}) {
    if (const int rc = test(); rc != 0) {
      return rc;
    }
  }
  return 0;
}
