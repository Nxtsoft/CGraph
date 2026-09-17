#include "cgraph/report.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cgraph {
namespace {

namespace fs = std::filesystem;

constexpr std::array<const char*, 4> kViewNames = {"modules", "design", "clones", "types"};
constexpr std::array<const char*, 4> kFormatNames = {"json", "mermaid", "svg", "markdown"};

// Directory names that mark a test root. Matched on every path component so a
// nested tests/ under a package is excluded the same as a top-level one.
constexpr std::array<std::string_view, 12> kTestDirs = {
    "test",     "tests",     "testing", "__tests__", "spec",      "specs",
    "e2e",      "testdata",  "fixtures", "__mocks__", "test_data", "integration_tests",
};

[[nodiscard]] std::string lowercase(std::string_view text) {
  std::string out(text);
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

[[nodiscard]] bool is_test_module(const std::string& name) {
  std::size_t start = 0;
  while (start <= name.size()) {
    const auto end = name.find('/', start);
    const auto part = lowercase(std::string_view(name).substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (std::find(kTestDirs.begin(), kTestDirs.end(), part) != kTestDirs.end()) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

[[nodiscard]] bool in_scope(const std::string& name, const std::string& scope) {
  return scope.empty() || name == scope || name.starts_with(scope + "/");
}

// Test roots are left out entirely unless asked for: as sources because they
// would own layer 0, and as targets because production code never depends on
// a test -- such an edge is a name-collision resolution artifact.
[[nodiscard]] bool is_admitted_module(const std::string& name, const ReportRequest& request) {
  return request.include_tests || !is_test_module(name);
}

[[nodiscard]] bool is_source_module(const std::string& name, const ReportRequest& request) {
  return in_scope(name, request.scope) && is_admitted_module(name, request);
}

// The first `depth` directory components of `source_file`, relative to `root`.
// A file at the root itself belongs to module ".".
[[nodiscard]] std::string module_for(const std::string& source_file, const fs::path& root, int depth) {
  fs::path path = fs::path(source_file).lexically_normal();
  if (!root.empty()) {
    const auto relative = path.lexically_relative(root);
    if (!relative.empty() && relative.generic_string() != "." && !relative.generic_string().starts_with("..")) {
      path = relative;
    }
  }
  std::string out;
  int taken = 0;
  for (const auto& part : path.parent_path()) {
    const auto text = part.generic_string();
    if (text.empty() || text == "/" || text == ".") {
      continue;
    }
    if (taken == depth) {
      break;
    }
    if (!out.empty()) {
      out += '/';
    }
    out += text;
    ++taken;
  }
  return out.empty() ? "." : out;
}

[[nodiscard]] std::string plural(std::size_t count, const char* noun) {
  return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

[[nodiscard]] std::string dependencies_caption(std::size_t count) {
  return std::to_string(count) + (count == 1 ? " dependency" : " dependencies");
}

[[nodiscard]] std::string edge_label(const ModuleDependency& edge) {
  std::string label;
  if (edge.calls > 0) {
    label += plural(edge.calls, "call");
  }
  if (edge.imports > 0) {
    if (!label.empty()) {
      label += ", ";
    }
    label += plural(edge.imports, "import");
  }
  return label;
}

[[nodiscard]] std::string xml_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// Mermaid node ids: [A-Za-z0-9_] only, never starting with a digit and never a
// reserved word ("end"), so every id is prefixed. Distinct names that collapse
// to one id ("a-b" and "a_b") are disambiguated by a numeric suffix.
[[nodiscard]] std::unordered_map<std::string, std::string> mermaid_ids(const ModulesReport& report) {
  std::unordered_map<std::string, std::string> ids;
  std::set<std::string> used;
  for (const auto& module : report.modules) {
    std::string id = "m_";
    for (const char c : module.name) {
      id += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
    std::string candidate = id;
    for (int suffix = 2; used.contains(candidate); ++suffix) {
      candidate = id + "_" + std::to_string(suffix);
    }
    used.insert(candidate);
    ids.emplace(module.name, candidate);
  }
  return ids;
}

[[nodiscard]] std::string mermaid_label(std::string_view value) {
  std::string out;
  for (const char c : value) {
    if (c == '"') {
      out += "#quot;";
    } else {
      out += c;
    }
  }
  return out;
}

[[nodiscard]] std::unordered_map<std::string, std::size_t> module_weights(const ModulesReport& report) {
  std::unordered_map<std::string, std::size_t> weight;
  for (const auto& module : report.modules) {
    weight.emplace(module.name, 0);
  }
  for (const auto& edge : report.edges) {
    weight[edge.from] += edge.weight();
    weight[edge.to] += edge.weight();
  }
  return weight;
}

[[nodiscard]] std::string scope_caption(const ModulesReport& report) {
  std::string caption = "depth " + std::to_string(report.depth);
  caption += report.scope.empty() ? std::string(", whole project") : ", scope " + report.scope;
  caption += report.include_tests ? ", tests included" : ", tests excluded";
  return caption;
}

[[nodiscard]] std::string omitted_caption(const ModulesReport& report) {
  return "omitted: " + plural(report.omitted_modules, "module") + ", " + plural(report.omitted_edges, "edge") + " (of " +
         plural(report.total_modules, "module") + ", " + plural(report.total_edges, "edge") + ")";
}

}  // namespace

const char* report_view_name(ReportView view) {
  return kViewNames[static_cast<std::size_t>(view)];
}

std::optional<ReportView> report_view_from_string(std::string_view name) {
  for (std::size_t i = 0; i < kViewNames.size(); ++i) {
    if (name == kViewNames[i]) {
      return static_cast<ReportView>(i);
    }
  }
  return std::nullopt;
}

const char* report_format_name(ReportFormat format) {
  return kFormatNames[static_cast<std::size_t>(format)];
}

std::optional<ReportFormat> report_format_from_string(std::string_view name) {
  for (std::size_t i = 0; i < kFormatNames.size(); ++i) {
    if (name == kFormatNames[i]) {
      return static_cast<ReportFormat>(i);
    }
  }
  return std::nullopt;
}

std::optional<std::string> parse_report_request(const nlohmann::json& params, ReportRequest& out) {
  const auto view_name = params.value("view", std::string{"modules"});
  const auto view = report_view_from_string(view_name);
  if (!view) {
    return "unknown report view '" + view_name + "' (expected modules|design|clones|types)";
  }
  out.view = *view;
  const auto format_name = params.value("format", std::string{"json"});
  const auto format = report_format_from_string(format_name);
  if (!format) {
    return "unknown report format '" + format_name + "' (expected json|mermaid|svg|markdown)";
  }
  out.format = *format;
  const auto budget = params.value("budget", static_cast<long long>(kDefaultReportBudget));
  if (budget < 0) {
    return "budget must be >= 0 (0 disables the budget)";
  }
  out.budget = static_cast<std::size_t>(budget);
  const auto depth = params.value("depth", kDefaultModuleDepth);
  if (depth < 1) {
    return "depth must be >= 1";
  }
  out.module_depth = depth;
  out.scope = params.value("scope", std::string{});
  while (!out.scope.empty() && out.scope.back() == '/') {
    out.scope.pop_back();
  }
  out.include_tests = params.value("include_tests", false);
  out.threshold = params.value("threshold", 0.80);
  if (!(out.threshold >= 0.0 && out.threshold <= 1.0)) {
    return "threshold must be between 0 and 1";
  }
  const auto min_tokens = params.value("min_tokens", static_cast<long long>(30));
  if (min_tokens < 0) {
    return "min_tokens must be >= 0";
  }
  out.min_tokens = static_cast<std::size_t>(min_tokens);
  const auto min_members = params.value("min_members", static_cast<long long>(kDefaultMinMembers));
  if (min_members < 1) {
    return "min_members must be >= 1";
  }
  out.min_members = static_cast<std::size_t>(min_members);
  const auto hops = params.value("hops", kDefaultHops);
  if (hops < 1) {
    return "hops must be >= 1";
  }
  out.hops = hops;
  return std::nullopt;
}

ModulesReport build_modules_report(const GraphSnapshot& graph, const ReportRequest& request) {
  ModulesReport report;
  report.depth = std::max(1, request.module_depth);
  report.scope = request.scope;
  report.include_tests = request.include_tests;

  fs::path root;
  if (!request.project_root.empty()) {
    std::error_code ec;
    root = fs::weakly_canonical(request.project_root, ec);
    if (ec) {
      root = request.project_root.lexically_normal();
    }
  }

  // Every code node joins the module of its source file. Enrichment and memory
  // nodes are prose about the code, not code, and stay out.
  struct Group {
    std::set<std::string> files;
    std::size_t symbols = 0;
  };
  std::map<std::string, Group> groups;
  std::unordered_map<std::string, std::string> module_of_file;
  std::unordered_map<std::string, std::string> module_of_node;
  module_of_node.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    if (node.source_file.empty() || is_enrichment_node_id(node.id) || is_memory_node_id(node.id)) {
      continue;
    }
    auto file_it = module_of_file.find(node.source_file);
    if (file_it == module_of_file.end()) {
      file_it = module_of_file.emplace(node.source_file, module_for(node.source_file, root, report.depth)).first;
    }
    const auto& module = file_it->second;
    module_of_node.emplace(node.id, module);
    auto& group = groups[module];
    group.files.insert(node.source_file);
    // A `field` is a member of a symbol, not a symbol of its own: nobody
    // navigates a module by its struct members, and counting them made a
    // module's symbol count jump the moment type members were extracted
    // (turing-webapp: 9,307 fields against 5,338 functions, classes and types).
    if (node.kind != "file" && node.kind != "field") {
      ++group.symbols;
    }
  }

  // Aggregate cross-module dependency edges. Only modules in scope report their
  // outgoing edges; the modules they reach are kept as targets.
  struct Agg {
    std::size_t calls = 0;
    std::size_t imports = 0;
  };
  std::map<std::pair<std::string, std::string>, Agg> aggregate;
  for (const auto& edge : graph.edges) {
    const bool call = edge.relation == "CALLS";
    const bool import = edge.relation == "imports" || edge.relation == "imports_from" || edge.relation == "re_exports";
    if (!call && !import) {
      continue;
    }
    const auto from = module_of_node.find(edge.source);
    const auto to = module_of_node.find(edge.target);
    if (from == module_of_node.end() || to == module_of_node.end() || from->second == to->second) {
      continue;
    }
    if (!is_source_module(from->second, request) || !is_admitted_module(to->second, request)) {
      continue;
    }
    auto& agg = aggregate[{from->second, to->second}];
    if (call) {
      ++agg.calls;
    } else {
      ++agg.imports;
    }
  }

  std::set<std::string> names;
  for (const auto& [name, group] : groups) {
    if (is_source_module(name, request)) {
      names.insert(name);
    }
  }
  for (const auto& [key, agg] : aggregate) {
    names.insert(key.second);
  }
  std::vector<std::string> ordered(names.begin(), names.end());
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    index.emplace(ordered[i], i);
  }
  const std::size_t n = ordered.size();
  std::vector<std::vector<std::size_t>> out(n);
  for (const auto& [key, agg] : aggregate) {
    out[index.at(key.first)].push_back(index.at(key.second));
  }

  // Tarjan's strongly connected components: every cycle collapses to one
  // component, so layering runs over a DAG and cycles are listed, not hidden.
  std::vector<int> component(n, -1);
  std::vector<int> order(n, -1);
  std::vector<int> low(n, 0);
  std::vector<bool> on_stack(n, false);
  std::vector<std::size_t> stack;
  int counter = 0;
  int components = 0;
  std::function<void(std::size_t)> strongconnect = [&](std::size_t v) {
    order[v] = low[v] = counter++;
    stack.push_back(v);
    on_stack[v] = true;
    for (const auto w : out[v]) {
      if (order[w] < 0) {
        strongconnect(w);
        low[v] = std::min(low[v], low[w]);
      } else if (on_stack[w]) {
        low[v] = std::min(low[v], order[w]);
      }
    }
    if (low[v] == order[v]) {
      while (true) {
        const auto w = stack.back();
        stack.pop_back();
        on_stack[w] = false;
        component[w] = components;
        if (w == v) {
          break;
        }
      }
      ++components;
    }
  };
  for (std::size_t v = 0; v < n; ++v) {
    if (order[v] < 0) {
      strongconnect(v);
    }
  }

  // Longest path over the condensation: a module's layer is the longest chain
  // of dependents above it, so layer 0 holds what nothing depends on.
  std::vector<std::set<int>> component_out(static_cast<std::size_t>(components));
  std::vector<int> indegree(static_cast<std::size_t>(components), 0);
  std::vector<std::size_t> component_size(static_cast<std::size_t>(components), 0);
  for (std::size_t v = 0; v < n; ++v) {
    ++component_size[static_cast<std::size_t>(component[v])];
    for (const auto w : out[v]) {
      if (component[v] != component[w]) {
        component_out[static_cast<std::size_t>(component[v])].insert(component[w]);
      }
    }
  }
  for (const auto& targets : component_out) {
    for (const auto t : targets) {
      ++indegree[static_cast<std::size_t>(t)];
    }
  }
  std::vector<int> layer(static_cast<std::size_t>(components), 0);
  std::vector<int> ready;
  for (int c = 0; c < components; ++c) {
    if (indegree[static_cast<std::size_t>(c)] == 0) {
      ready.push_back(c);
    }
  }
  while (!ready.empty()) {
    const int c = ready.back();
    ready.pop_back();
    for (const auto t : component_out[static_cast<std::size_t>(c)]) {
      auto& target_layer = layer[static_cast<std::size_t>(t)];
      target_layer = std::max(target_layer, layer[static_cast<std::size_t>(c)] + 1);
      if (--indegree[static_cast<std::size_t>(t)] == 0) {
        ready.push_back(t);
      }
    }
  }

  for (std::size_t i = 0; i < n; ++i) {
    const auto group = groups.find(ordered[i]);
    const auto c = static_cast<std::size_t>(component[i]);
    report.modules.push_back(ModuleSummary{
        .name = ordered[i],
        .files = group == groups.end() ? 0 : group->second.files.size(),
        .symbols = group == groups.end() ? 0 : group->second.symbols,
        .layer = layer[c],
        .in_cycle = component_size[c] > 1,
    });
  }
  for (const auto& [key, agg] : aggregate) {
    report.edges.push_back(ModuleDependency{
        .from = key.first,
        .to = key.second,
        .calls = agg.calls,
        .imports = agg.imports,
        .cycle = component[index.at(key.first)] == component[index.at(key.second)],
    });
  }

  const auto weight = module_weights(report);
  std::sort(report.modules.begin(), report.modules.end(), [&](const ModuleSummary& a, const ModuleSummary& b) {
    if (a.layer != b.layer) {
      return a.layer < b.layer;
    }
    if (weight.at(a.name) != weight.at(b.name)) {
      return weight.at(a.name) > weight.at(b.name);
    }
    return a.name < b.name;
  });
  std::sort(report.edges.begin(), report.edges.end(), [](const ModuleDependency& a, const ModuleDependency& b) {
    if (a.weight() != b.weight()) {
      return a.weight() > b.weight();
    }
    return std::tie(a.from, a.to) < std::tie(b.from, b.to);
  });

  int max_layer = -1;
  for (const auto& module : report.modules) {
    max_layer = std::max(max_layer, module.layer);
  }
  report.layers.assign(static_cast<std::size_t>(max_layer + 1), {});
  for (const auto& module : report.modules) {
    report.layers[static_cast<std::size_t>(module.layer)].push_back(module.name);
  }

  std::map<int, std::vector<std::string>> members;
  for (std::size_t i = 0; i < n; ++i) {
    if (component_size[static_cast<std::size_t>(component[i])] > 1) {
      members[component[i]].push_back(ordered[i]);
    }
  }
  for (auto& [c, cycle_members] : members) {
    std::sort(cycle_members.begin(), cycle_members.end());
    report.cycles.push_back(std::move(cycle_members));
  }
  std::sort(report.cycles.begin(), report.cycles.end());

  report.total_modules = report.modules.size();
  report.total_edges = report.edges.size();
  return report;
}

std::size_t estimate_report_tokens(std::string_view text) {
  return estimate_report_tokens(text.size());
}

std::size_t estimate_report_tokens(std::size_t byte_length) {
  return (byte_length + 3) / 4;
}

void shed_to_budget(ModulesReport& report, ReportFormat format, std::size_t budget) {
  if (budget == 0) {
    return;
  }
  const auto fits = [&](const ModulesReport& candidate) {
    return estimate_report_tokens(render_modules_report(candidate, format)) <= budget;
  };
  if (fits(report)) {
    return;
  }

  // Phase 1: edges are heaviest-first, so keeping a prefix sheds the lightest.
  const auto with_edges = [&](std::size_t keep) {
    ModulesReport candidate = report;
    candidate.edges.resize(keep);
    candidate.omitted_edges = report.total_edges - keep;
    return candidate;
  };
  if (fits(with_edges(0))) {
    std::size_t lo = 0;
    std::size_t hi = report.edges.size();
    while (lo < hi) {
      const auto mid = (lo + hi + 1) / 2;
      if (fits(with_edges(mid))) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    report = with_edges(lo);
    return;
  }

  // Phase 2: even the bare module list overflows, so shed modules lightest
  // first; an edge leaves with either endpoint.
  const auto weight = module_weights(report);
  std::vector<ModuleSummary> ranking = report.modules;
  std::sort(ranking.begin(), ranking.end(), [&](const ModuleSummary& a, const ModuleSummary& b) {
    if (weight.at(a.name) != weight.at(b.name)) {
      return weight.at(a.name) > weight.at(b.name);
    }
    if (a.files != b.files) {
      return a.files > b.files;
    }
    return a.name < b.name;
  });
  const auto with_modules = [&](std::size_t keep) {
    std::set<std::string> kept;
    for (std::size_t i = 0; i < keep; ++i) {
      kept.insert(ranking[i].name);
    }
    ModulesReport candidate = report;
    candidate.modules.clear();
    candidate.edges.clear();
    candidate.layers.clear();
    candidate.cycles.clear();
    for (const auto& module : report.modules) {
      if (kept.contains(module.name)) {
        candidate.modules.push_back(module);
      }
    }
    for (const auto& edge : report.edges) {
      if (kept.contains(edge.from) && kept.contains(edge.to)) {
        candidate.edges.push_back(edge);
      }
    }
    // Layers stay positional (index = layer) so a kept module's layer label
    // is still true; a fully shed layer is left empty and renderers skip it.
    for (const auto& layer : report.layers) {
      std::vector<std::string> kept_layer;
      for (const auto& name : layer) {
        if (kept.contains(name)) {
          kept_layer.push_back(name);
        }
      }
      candidate.layers.push_back(std::move(kept_layer));
    }
    for (const auto& cycle : report.cycles) {
      std::vector<std::string> kept_cycle;
      for (const auto& name : cycle) {
        if (kept.contains(name)) {
          kept_cycle.push_back(name);
        }
      }
      if (kept_cycle.size() > 1) {
        candidate.cycles.push_back(std::move(kept_cycle));
      }
    }
    candidate.omitted_modules = report.total_modules - keep;
    candidate.omitted_edges = report.total_edges - candidate.edges.size();
    return candidate;
  };
  std::size_t lo = 0;
  std::size_t hi = ranking.size();
  while (lo < hi) {
    const auto mid = (lo + hi + 1) / 2;
    if (fits(with_modules(mid))) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  report = with_modules(lo);
}

nlohmann::json modules_report_json(const ModulesReport& report) {
  nlohmann::json modules = nlohmann::json::array();
  for (const auto& module : report.modules) {
    modules.push_back({{"name", module.name}, {"files", module.files}, {"symbols", module.symbols}, {"layer", module.layer}});
  }
  nlohmann::json edges = nlohmann::json::array();
  for (const auto& edge : report.edges) {
    nlohmann::json row = {{"from", edge.from}, {"to", edge.to}, {"calls", edge.calls}, {"imports", edge.imports}};
    if (edge.cycle) {
      row["cycle"] = true;
    }
    edges.push_back(std::move(row));
  }
  return nlohmann::json{
      {"view", "modules"},
      {"depth", report.depth},
      {"scope", report.scope},
      {"include_tests", report.include_tests},
      {"modules", std::move(modules)},
      {"edges", std::move(edges)},
      {"layers", report.layers},
      {"cycles", report.cycles},
      {"totals", {{"modules", report.total_modules}, {"edges", report.total_edges}}},
      {"omitted", {{"modules", report.omitted_modules}, {"edges", report.omitted_edges}}},
  };
}

std::string render_modules_mermaid(const ModulesReport& report) {
  const auto ids = mermaid_ids(report);
  std::string out = "graph LR\n";
  out += "  %% modules: " + scope_caption(report) + "; layers left to right\n";
  for (std::size_t layer = 0; layer < report.layers.size(); ++layer) {
    if (report.layers[layer].empty()) {
      continue;
    }
    out += "  subgraph layer" + std::to_string(layer) + "[\"layer " + std::to_string(layer) + "\"]\n";
    for (const auto& name : report.layers[layer]) {
      const auto module = std::find_if(report.modules.begin(), report.modules.end(),
                                       [&](const ModuleSummary& m) { return m.name == name; });
      const auto files = module == report.modules.end() ? 0 : module->files;
      out += "    " + ids.at(name) + "[\"" + mermaid_label(name) + "<br/>" + plural(files, "file") + "\"]\n";
    }
    out += "  end\n";
  }
  for (const auto& edge : report.edges) {
    out += "  " + ids.at(edge.from) + (edge.cycle ? " -.->|\"" : " -->|\"") + edge_label(edge) + "\"| " + ids.at(edge.to) + "\n";
  }
  if (report.cycles.empty()) {
    out += "  %% cycles: none\n";
  }
  for (const auto& cycle : report.cycles) {
    out += "  %% cycle:";
    for (const auto& name : cycle) {
      out += " " + name;
    }
    out += "\n";
  }
  out += "  %% " + omitted_caption(report) + "\n";
  return out;
}

std::string render_modules_markdown(const ModulesReport& report) {
  std::string out = "# Module dependencies\n\n";
  out += scope_caption(report) + " · " + plural(report.modules.size(), "module") + " · " +
         dependencies_caption(report.edges.size()) + "\n\n";
  out += "## Layers\n\n";
  for (std::size_t layer = 0; layer < report.layers.size(); ++layer) {
    if (report.layers[layer].empty()) {
      continue;
    }
    out += "- layer " + std::to_string(layer) + ":";
    for (std::size_t i = 0; i < report.layers[layer].size(); ++i) {
      out += (i == 0 ? " `" : ", `") + report.layers[layer][i] + "`";
    }
    out += "\n";
  }
  out += "\n## Dependencies\n\n| from | to | calls | imports |\n| --- | --- | ---: | ---: |\n";
  for (const auto& edge : report.edges) {
    out += "| " + edge.from + " | " + edge.to + " | " + std::to_string(edge.calls) + " | " + std::to_string(edge.imports) +
           (edge.cycle ? " | (cycle)\n" : " |\n");
  }
  out += "\n## Cycles\n\n";
  if (report.cycles.empty()) {
    out += "none\n";
  }
  for (const auto& cycle : report.cycles) {
    out += "-";
    for (std::size_t i = 0; i < cycle.size(); ++i) {
      out += (i == 0 ? " `" : " <-> `") + cycle[i] + "`";
    }
    out += "\n";
  }
  out += "\n" + omitted_caption(report) + "\n";
  return out;
}

std::string render_modules_svg(const ModulesReport& report) {
  constexpr double kBoxHeight = 46.0;
  constexpr double kRowGap = 24.0;
  constexpr double kColumnGap = 170.0;
  constexpr double kMarginX = 32.0;
  constexpr double kMarginTop = 84.0;
  constexpr double kCharWidth = 7.4;
  constexpr double kPadX = 16.0;
  constexpr double kMinBoxWidth = 128.0;
  const char* kFont = "-apple-system, BlinkMacSystemFont, Segoe UI, Helvetica, Arial, sans-serif";

  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < report.modules.size(); ++i) {
    index.emplace(report.modules[i].name, i);
  }
  // Columns are layers; within a column, modules start in report order
  // (heaviest first) and are then reordered by the barycenter of their
  // neighbors in adjacent columns so edges cross as little as a sweep affords.
  // A cycle's members share one semantic layer, but drawn in one column their
  // edges become a pile of loops. For drawing only, each cycle is spread into
  // sub-columns: members are ordered by net outgoing weight (Eades-Lin-Smyth
  // style, so the edges pointing along the order are the many and the back
  // edges the few), then ranked by longest path over the forward edges.
  std::vector<int> sub_rank(report.modules.size(), 0);
  for (const auto& cycle : report.cycles) {
    std::vector<std::size_t> members;
    for (const auto& name : cycle) {
      if (const auto it = index.find(name); it != index.end()) {
        members.push_back(it->second);
      }
    }
    std::unordered_map<std::size_t, long long> net;
    for (const auto m : members) {
      net.emplace(m, 0);
    }
    for (const auto& edge : report.edges) {
      const auto from = index.find(edge.from);
      const auto to = index.find(edge.to);
      if (from == index.end() || to == index.end() || !net.contains(from->second) || !net.contains(to->second)) {
        continue;
      }
      net[from->second] += static_cast<long long>(edge.weight());
      net[to->second] -= static_cast<long long>(edge.weight());
    }
    std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
      if (net.at(a) != net.at(b)) {
        return net.at(a) > net.at(b);
      }
      return report.modules[a].name < report.modules[b].name;
    });
    std::unordered_map<std::size_t, std::size_t> position;
    for (std::size_t i = 0; i < members.size(); ++i) {
      position.emplace(members[i], i);
    }
    // Longest path over forward edges, in order (a forward edge always points
    // to a later member, so one pass in member order suffices).
    for (const auto m : members) {
      for (const auto& edge : report.edges) {
        const auto from = index.find(edge.from);
        const auto to = index.find(edge.to);
        if (from == index.end() || to == index.end() || from->second != m || !position.contains(to->second)) {
          continue;
        }
        if (position.at(to->second) > position.at(m)) {
          sub_rank[to->second] = std::max(sub_rank[to->second], sub_rank[m] + 1);
        }
      }
    }
  }

  // Columns: one per semantic layer, widened by that layer's largest sub-rank.
  std::vector<int> column_layer;  // column index -> layer number (-1 = continuation of the previous layer)
  std::unordered_map<int, std::size_t> first_column_of_layer;
  for (std::size_t layer = 0; layer < report.layers.size(); ++layer) {
    if (report.layers[layer].empty()) {
      continue;
    }
    int width = 1;
    for (const auto& name : report.layers[layer]) {
      width = std::max(width, sub_rank[index.at(name)] + 1);
    }
    first_column_of_layer.emplace(static_cast<int>(layer), column_layer.size());
    column_layer.push_back(static_cast<int>(layer));
    for (int extra = 1; extra < width; ++extra) {
      column_layer.push_back(-1);
    }
  }
  std::vector<std::vector<std::size_t>> columns(column_layer.size());
  std::vector<std::size_t> column_of(report.modules.size(), 0);
  for (std::size_t i = 0; i < report.modules.size(); ++i) {
    column_of[i] = first_column_of_layer.at(report.modules[i].layer) + static_cast<std::size_t>(sub_rank[i]);
    columns[column_of[i]].push_back(i);
  }
  std::vector<std::vector<std::size_t>> predecessors(report.modules.size());
  std::vector<std::vector<std::size_t>> successors(report.modules.size());
  for (const auto& edge : report.edges) {
    const auto from = index.find(edge.from);
    const auto to = index.find(edge.to);
    if (from == index.end() || to == index.end()) {
      continue;
    }
    successors[from->second].push_back(to->second);
    predecessors[to->second].push_back(from->second);
  }
  std::vector<double> row(report.modules.size(), 0.0);
  const auto assign_rows = [&]() {
    for (const auto& column : columns) {
      for (std::size_t r = 0; r < column.size(); ++r) {
        row[column[r]] = static_cast<double>(r);
      }
    }
  };
  const auto sweep = [&](std::vector<std::size_t>& column, const std::vector<std::vector<std::size_t>>& neighbors) {
    std::vector<std::pair<double, std::size_t>> keyed;
    for (const auto module : column) {
      double sum = 0.0;
      std::size_t count = 0;
      for (const auto neighbor : neighbors[module]) {
        sum += row[neighbor];
        ++count;
      }
      keyed.emplace_back(count == 0 ? row[module] : sum / static_cast<double>(count), module);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t r = 0; r < column.size(); ++r) {
      column[r] = keyed[r].second;
    }
  };
  assign_rows();
  for (int pass = 0; pass < 2; ++pass) {
    for (std::size_t c = 1; c < columns.size(); ++c) {
      sweep(columns[c], predecessors);
      assign_rows();
    }
    for (std::size_t c = columns.size(); c-- > 1;) {
      sweep(columns[c - 1], successors);
      assign_rows();
    }
  }

  std::vector<double> column_width(columns.size(), kMinBoxWidth);
  std::size_t max_rows = 0;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    max_rows = std::max(max_rows, columns[c].size());
    for (const auto module : columns[c]) {
      const auto& summary = report.modules[module];
      const auto subtitle = plural(summary.files, "file") + " · " + plural(summary.symbols, "symbol");
      const auto chars = std::max(summary.name.size(), subtitle.size() * 4 / 5);
      column_width[c] = std::max(column_width[c], static_cast<double>(chars) * kCharWidth + 2 * kPadX);
    }
  }
  std::vector<double> column_x(columns.size(), kMarginX);
  for (std::size_t c = 1; c < columns.size(); ++c) {
    column_x[c] = column_x[c - 1] + column_width[c - 1] + kColumnGap;
  }
  const double rows_height = static_cast<double>(std::max<std::size_t>(max_rows, 1)) * (kBoxHeight + kRowGap);
  const double width = columns.empty() ? 640.0 : column_x.back() + column_width.back() + kMarginX;
  const double height = kMarginTop + rows_height + 16.0;

  std::vector<double> box_x(report.modules.size(), 0.0);
  std::vector<double> box_y(report.modules.size(), 0.0);
  std::vector<double> box_w(report.modules.size(), 0.0);
  for (std::size_t c = 0; c < columns.size(); ++c) {
    const double offset = (static_cast<double>(max_rows - columns[c].size()) * (kBoxHeight + kRowGap)) / 2.0;
    for (std::size_t r = 0; r < columns[c].size(); ++r) {
      const auto module = columns[c][r];
      box_x[module] = column_x[c];
      box_y[module] = kMarginTop + offset + static_cast<double>(r) * (kBoxHeight + kRowGap);
      box_w[module] = column_width[c];
    }
  }

  std::ostringstream svg;
  svg.setf(std::ios::fixed);
  svg.precision(1);
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << width << " " << height << "\" width=\"" << width
      << "\" height=\"" << height << "\" font-family=\"" << kFont << "\">\n";
  svg << "<defs>"
         "<marker id=\"arrow\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"11\" markerHeight=\"11\" "
         "markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M0 0L10 5L0 10z\" fill=\"#64748b\"/></marker>"
         "<marker id=\"arrow-cycle\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"11\" markerHeight=\"11\" "
         "markerUnits=\"userSpaceOnUse\" orient=\"auto\"><path d=\"M0 0L10 5L0 10z\" fill=\"#dc2626\"/></marker>"
         "</defs>\n";
  svg << "<rect width=\"" << width << "\" height=\"" << height << "\" fill=\"#ffffff\"/>\n";
  svg << "<text x=\"" << kMarginX << "\" y=\"30\" font-size=\"17\" font-weight=\"600\" fill=\"#0f172a\">Module dependencies · "
      << xml_escape(scope_caption(report)) << "</text>\n";
  std::string cycles_caption = report.cycles.empty() ? "cycles: none" : "cycles: " + std::to_string(report.cycles.size());
  svg << "<text x=\"" << kMarginX << "\" y=\"52\" font-size=\"12\" fill=\"#475569\">" << plural(report.modules.size(), "module")
      << " · " << dependencies_caption(report.edges.size()) << " · " << cycles_caption
      << " · " << xml_escape(omitted_caption(report)) << "</text>\n";
  if (report.modules.empty()) {
    svg << "<text x=\"" << kMarginX << "\" y=\"" << kMarginTop << "\" font-size=\"13\" fill=\"#64748b\">no modules in scope</text>\n";
  }
  for (std::size_t c = 0; c < columns.size(); ++c) {
    if (column_layer[c] < 0) {
      continue;  // a cycle's extra sub-column: same layer as the column before it
    }
    svg << "<text x=\"" << column_x[c] << "\" y=\"" << (kMarginTop - 12.0) << "\" font-size=\"11\" fill=\"#94a3b8\">layer "
        << column_layer[c] << "</text>\n";
  }

  // Edges leaving one box share its row, so their labels are staggered along
  // the curve: the heaviest sits nearest the source, the next further along.
  std::unordered_map<std::string, int> labels_from;
  for (const auto& edge : report.edges) {
    const auto from = index.find(edge.from);
    const auto to = index.find(edge.to);
    if (from == index.end() || to == index.end()) {
      continue;
    }
    const auto s = from->second;
    const auto t = to->second;
    const int label_slot = labels_from[edge.from]++;
    const double sy = box_y[s] + kBoxHeight / 2.0;
    const double ty = box_y[t] + kBoxHeight / 2.0;
    double x0 = 0.0;
    double x1 = 0.0;
    double x2 = 0.0;
    double x3 = 0.0;
    const double bend = kColumnGap * 0.5;
    if (column_of[t] > column_of[s]) {
      x0 = box_x[s] + box_w[s];
      x3 = box_x[t];
      x1 = x0 + bend;
      x2 = x3 - bend;
    } else if (column_of[t] < column_of[s]) {
      x0 = box_x[s];
      x3 = box_x[t] + box_w[t];
      x1 = x0 - bend;
      x2 = x3 + bend;
    } else {
      x0 = box_x[s] + box_w[s];
      x3 = box_x[t] + box_w[t];
      x1 = x0 + bend;
      x2 = x3 + bend;
    }
    const double stroke = 1.0 + 0.7 * std::log2(1.0 + static_cast<double>(edge.weight()));
    svg << "<path d=\"M" << x0 << " " << sy << " C" << x1 << " " << sy << ", " << x2 << " " << ty << ", " << x3 << " " << ty
        << "\" fill=\"none\" stroke=\"" << (edge.cycle ? "#dc2626" : "#64748b") << "\" stroke-width=\"" << stroke
        << "\" stroke-opacity=\"0.85\"" << (edge.cycle ? " stroke-dasharray=\"6 4\"" : "") << " marker-end=\"url(#"
        << (edge.cycle ? "arrow-cycle" : "arrow") << ")\"/>\n";
    // Label toward the source end of the bezier: edges fanning into one hub
    // then carry their labels at their own source's row instead of stacking at
    // the shared midpoint, and siblings from one source spread along the curve.
    const double kLabelT = std::min(0.72, 0.28 + 0.22 * static_cast<double>(label_slot));
    const double kLabelU = 1.0 - kLabelT;
    const double mx = kLabelU * kLabelU * kLabelU * x0 + 3.0 * kLabelU * kLabelU * kLabelT * x1 +
                      3.0 * kLabelU * kLabelT * kLabelT * x2 + kLabelT * kLabelT * kLabelT * x3;
    const double my = kLabelU * kLabelU * kLabelU * sy + 3.0 * kLabelU * kLabelU * kLabelT * sy +
                      3.0 * kLabelU * kLabelT * kLabelT * ty + kLabelT * kLabelT * kLabelT * ty;
    svg << "<text x=\"" << mx << "\" y=\"" << (my - 4.0) << "\" font-size=\"10.5\" text-anchor=\"middle\" fill=\""
        << (edge.cycle ? "#b91c1c" : "#334155") << "\" style=\"paint-order:stroke;stroke:#ffffff;stroke-width:3px\">"
        << xml_escape(edge_label(edge)) << "</text>\n";
  }

  for (std::size_t i = 0; i < report.modules.size(); ++i) {
    const auto& module = report.modules[i];
    svg << "<rect x=\"" << box_x[i] << "\" y=\"" << box_y[i] << "\" width=\"" << box_w[i] << "\" height=\"" << kBoxHeight
        << "\" rx=\"8\" fill=\"" << (module.in_cycle ? "#fef2f2" : "#eef2ff") << "\" stroke=\""
        << (module.in_cycle ? "#dc2626" : "#4f46e5") << "\" stroke-width=\"1.5\"/>\n";
    svg << "<text x=\"" << (box_x[i] + kPadX) << "\" y=\"" << (box_y[i] + 19.0) << "\" font-size=\"13\" font-weight=\"600\" fill=\"#1e1b4b\">"
        << xml_escape(module.name) << "</text>\n";
    svg << "<text x=\"" << (box_x[i] + kPadX) << "\" y=\"" << (box_y[i] + 36.0) << "\" font-size=\"11\" fill=\"#475569\">"
        << plural(module.files, "file") << " · " << plural(module.symbols, "symbol") << "</text>\n";
  }
  svg << "</svg>\n";
  return svg.str();
}

// ---- types view ---------------------------------------------------------------

namespace {

// Edges that build a type rather than use it. Everything else pointing at a
// type -- references, inherits, implements, imports, CALLS (a constructor),
// impl_trait, dispatches_to -- is a use.
[[nodiscard]] bool is_structural_relation(std::string_view relation) {
  return relation == "contains" || relation == "defines" || relation == "method" || relation == "method_of";
}

// A contract `schema` (an OpenAPI component, a proto message, a GraphQL type) is
// a type with members like any other: the types view then finds a hand-written
// TypeScript mirror of an API schema as an identical or overlapping shape.
[[nodiscard]] bool is_type_kind(std::string_view kind) {
  return kind == "class" || kind == "type" || kind == "schema";
}

// A contained pair is a `subset` row only when the smaller type is at least
// half of the larger one (Jaccard >= 0.5): `Base` missing one field of
// `Extended`, not `{id, name, createdAt}` inside a 30-member record. On a
// 1,800-type TypeScript app the bare containment rule produced 923 rows.
constexpr double kSubsetFloor = 0.5;
// Markdown lists this many declarations per duplicate row before "+N more";
// a component-local `Props` name declared in 26 files is one row, not a page.
constexpr std::size_t kMarkdownDeclarationCap = 8;

[[nodiscard]] std::size_t shared_members(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  std::size_t shared = 0;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      ++shared;
      ++i;
      ++j;
    } else if (a[i] < b[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return shared;
}

// |A ∩ B| / |A ∪ B|; two empty sets share nothing we can see, so 0.
[[nodiscard]] double member_jaccard(const std::vector<std::string>& a, const std::vector<std::string>& b, std::size_t shared) {
  const auto union_size = a.size() + b.size() - shared;
  return union_size == 0 ? 0.0 : static_cast<double>(shared) / static_cast<double>(union_size);
}

// Root-relative when the file lies under the project root, as module names are;
// an absolute path per row would spend the budget on the same prefix.
[[nodiscard]] std::string relative_file(const std::string& source_file, const fs::path& root) {
  if (root.empty()) {
    return source_file;
  }
  const auto relative = fs::path(source_file).lexically_normal().lexically_relative(root);
  const auto text = relative.generic_string();
  if (relative.empty() || text == "." || text.starts_with("..")) {
    return source_file;
  }
  return text;
}

[[nodiscard]] std::string file_line(const TypeRef& type) {
  return type.source_file + (type.line > 0 ? ":" + std::to_string(type.line) : std::string{});
}

[[nodiscard]] std::string join_members(const std::vector<std::string>& members, std::size_t cap = 8) {
  std::string out;
  for (std::size_t i = 0; i < members.size() && i < cap; ++i) {
    out += (i == 0 ? "" : ", ") + members[i];
  }
  if (members.size() > cap) {
    out += ", +" + std::to_string(members.size() - cap) + " more";
  }
  return out;
}

[[nodiscard]] std::string format_ratio(double value) {
  std::ostringstream out;
  out.precision(2);
  out << std::fixed << value;
  return out.str();
}

[[nodiscard]] std::string types_caption(const TypesReport& report) {
  std::string caption = report.scope.empty() ? std::string("whole project") : "scope " + report.scope;
  caption += report.include_tests ? ", tests included" : ", tests excluded";
  caption += ", threshold " + format_ratio(report.threshold) + ", min_members " + std::to_string(report.min_members);
  return caption;
}

[[nodiscard]] std::string types_omitted_caption(const TypesReport& report) {
  return "omitted: " + std::to_string(report.omitted_identical) + " identical, " + std::to_string(report.omitted_duplicates) +
         " duplicates, " + std::to_string(report.omitted_overlaps) + " overlaps, " + std::to_string(report.omitted_unreferenced) +
         " unreferenced (of " + std::to_string(report.total_identical) + ", " + std::to_string(report.total_duplicates) + ", " +
         std::to_string(report.total_overlaps) + ", " + std::to_string(report.total_unreferenced) + ")";
}

[[nodiscard]] nlohmann::json type_ref_json(const TypeRef& type) {
  nlohmann::json out{{"id", type.id}, {"label", type.label}, {"kind", type.kind}, {"file", type.source_file},
                     {"members", type.members}};
  if (type.line > 0) {
    out["line"] = type.line;
  }
  return out;
}

[[nodiscard]] bool type_ref_before(const TypeRef& a, const TypeRef& b) {
  return std::tie(a.label, a.source_file, a.line) < std::tie(b.label, b.source_file, b.line);
}

}  // namespace

TypesReport build_types_report(const GraphSnapshot& graph, const ReportRequest& request) {
  TypesReport report;
  report.scope = request.scope;
  report.include_tests = request.include_tests;
  report.threshold = request.threshold;
  report.min_members = std::max<std::size_t>(1, request.min_members);

  fs::path root;
  if (!request.project_root.empty()) {
    std::error_code ec;
    root = fs::weakly_canonical(request.project_root, ec);
    if (ec) {
      root = request.project_root.lexically_normal();
    }
  }

  // Candidate types: class/type nodes with a source file, in scope, outside
  // test roots unless asked. The directory path (every component) is what the
  // scope and test-root rules see, exactly as the modules view groups files.
  std::vector<TypeRef> types;
  std::unordered_map<std::string, std::size_t> index_of;
  std::unordered_map<std::string, std::string> directory_of_file;
  for (const auto& node : graph.nodes) {
    if (!is_type_kind(node.kind) || node.source_file.empty() || is_enrichment_node_id(node.id) ||
        is_memory_node_id(node.id)) {
      continue;
    }
    auto dir_it = directory_of_file.find(node.source_file);
    if (dir_it == directory_of_file.end()) {
      dir_it = directory_of_file.emplace(node.source_file, module_for(node.source_file, root, 1 << 20)).first;
    }
    if (!is_source_module(dir_it->second, request)) {
      continue;
    }
    index_of.emplace(node.id, types.size());
    types.push_back(TypeRef{
        .id = node.id,
        .label = node.label,
        .kind = node.kind,
        .source_file = relative_file(node.source_file, root),
        .line = node.source_location ? node.source_location->start_line : 0,
    });
  }
  report.total_types = types.size();

  // Members from `defines` edges to field nodes; uses from every other
  // non-structural edge into the type, excluding the type's own members.
  std::unordered_map<std::string, const Node*> by_id;
  by_id.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    by_id.emplace(node.id, &node);
  }
  std::unordered_map<std::string, std::string> owner_of_field;
  for (const auto& edge : graph.edges) {
    if (edge.relation != "defines") {
      continue;
    }
    const auto owner = index_of.find(edge.source);
    const auto field = by_id.find(edge.target);
    if (owner == index_of.end() || field == by_id.end() || field->second->kind != "field") {
      continue;
    }
    types[owner->second].members.push_back(field->second->label);
    owner_of_field.emplace(edge.target, edge.source);
  }
  for (auto& type : types) {
    std::sort(type.members.begin(), type.members.end());
    type.members.erase(std::unique(type.members.begin(), type.members.end()), type.members.end());
    if (!type.members.empty()) {
      ++report.total_with_members;
    }
  }
  for (const auto& edge : graph.edges) {
    const auto target = index_of.find(edge.target);
    if (target == index_of.end() || is_structural_relation(edge.relation) || edge.source == edge.target) {
      continue;
    }
    if (const auto owner = owner_of_field.find(edge.source); owner != owner_of_field.end() && owner->second == edge.target) {
      continue;  // a field pointing back at its own type is not a use
    }
    ++types[target->second].incoming;
  }

  // Duplicates: one label declared in two or more files. Most alike first, so
  // a type copied between files leads and a homonym convention (`Props` in
  // every component file) sinks.
  std::map<std::string, std::vector<std::size_t>> by_label;
  for (std::size_t i = 0; i < types.size(); ++i) {
    by_label[types[i].label].push_back(i);
  }
  for (auto& [label, indices] : by_label) {
    std::set<std::string> files;
    for (const auto i : indices) {
      files.insert(types[i].source_file);
    }
    if (files.size() < 2) {
      continue;
    }
    DuplicateTypes duplicate;
    duplicate.label = label;
    std::sort(indices.begin(), indices.end(), [&](std::size_t x, std::size_t y) {
      return std::tie(types[x].source_file, types[x].line) < std::tie(types[y].source_file, types[y].line);
    });
    for (const auto i : indices) {
      duplicate.declarations.push_back(types[i]);
    }
    duplicate.min_jaccard = 1.0;
    duplicate.max_jaccard = 0.0;
    for (std::size_t x = 0; x < indices.size(); ++x) {
      for (std::size_t y = x + 1; y < indices.size(); ++y) {
        const auto& a = types[indices[x]].members;
        const auto& b = types[indices[y]].members;
        const auto jaccard = member_jaccard(a, b, shared_members(a, b));
        duplicate.min_jaccard = std::min(duplicate.min_jaccard, jaccard);
        duplicate.max_jaccard = std::max(duplicate.max_jaccard, jaccard);
      }
    }
    report.duplicates.push_back(std::move(duplicate));
  }
  std::sort(report.duplicates.begin(), report.duplicates.end(), [](const DuplicateTypes& a, const DuplicateTypes& b) {
    if (a.min_jaccard != b.min_jaccard) {
      return a.min_jaccard > b.min_jaccard;
    }
    if (a.declarations.size() != b.declarations.size()) {
      return a.declarations.size() > b.declarations.size();
    }
    return a.label < b.label;
  });

  // Shape comparison among differently named types with at least min_members
  // members. Candidate pairs come from an inverted index on member label, so
  // only types with something in common are compared. Equal member sets are
  // grouped (union-find over identical pairs); nested sets are `subset` rows
  // when the smaller is at least half the larger; the rest need Jaccard >=
  // threshold to be an `overlap` row.
  std::unordered_map<std::string, std::vector<std::size_t>> types_with_member;
  for (std::size_t i = 0; i < types.size(); ++i) {
    if (types[i].members.size() < report.min_members) {
      continue;
    }
    for (const auto& member : types[i].members) {
      types_with_member[member].push_back(i);
    }
  }
  std::set<std::pair<std::size_t, std::size_t>> pairs;
  for (const auto& [member, indices] : types_with_member) {
    for (std::size_t x = 0; x < indices.size(); ++x) {
      for (std::size_t y = x + 1; y < indices.size(); ++y) {
        pairs.emplace(std::min(indices[x], indices[y]), std::max(indices[x], indices[y]));
      }
    }
  }
  std::vector<std::size_t> group_of(types.size());
  for (std::size_t i = 0; i < types.size(); ++i) {
    group_of[i] = i;
  }
  const auto find_group = [&](std::size_t i) {
    while (group_of[i] != i) {
      group_of[i] = group_of[group_of[i]];
      i = group_of[i];
    }
    return i;
  };
  for (const auto& [x, y] : pairs) {
    const auto& a = types[x];
    const auto& b = types[y];
    if (a.label == b.label) {
      continue;  // that is a duplicate, reported above
    }
    const auto shared = shared_members(a.members, b.members);
    const auto jaccard = member_jaccard(a.members, b.members, shared);
    if (shared == a.members.size() && shared == b.members.size()) {
      group_of[find_group(x)] = find_group(y);
      continue;
    }
    std::string relation;
    if (shared == std::min(a.members.size(), b.members.size()) && jaccard >= kSubsetFloor) {
      relation = "subset";
    } else if (jaccard >= report.threshold) {
      relation = "overlap";
    } else {
      continue;
    }
    TypeOverlap overlap;
    overlap.shared = shared;
    overlap.jaccard = jaccard;
    overlap.relation = relation;
    // The smaller type leads a subset; otherwise the lexically earlier one.
    const bool a_first = relation == "subset" ? a.members.size() <= b.members.size() : type_ref_before(a, b);
    overlap.a = a_first ? a : b;
    overlap.b = a_first ? b : a;
    report.overlaps.push_back(std::move(overlap));
  }
  std::map<std::size_t, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < types.size(); ++i) {
    if (types[i].members.size() >= report.min_members && find_group(i) != i) {
      groups[find_group(i)].push_back(i);
    }
  }
  for (auto& [leader, members] : groups) {
    members.push_back(leader);
    IdenticalTypes group;
    group.shape = types[leader].members;
    for (const auto i : members) {
      group.types.push_back(types[i]);
    }
    std::sort(group.types.begin(), group.types.end(), type_ref_before);
    report.identical.push_back(std::move(group));
  }
  std::sort(report.identical.begin(), report.identical.end(), [](const IdenticalTypes& x, const IdenticalTypes& y) {
    if (x.shape.size() != y.shape.size()) {
      return x.shape.size() > y.shape.size();
    }
    if (x.types.size() != y.types.size()) {
      return x.types.size() > y.types.size();
    }
    return type_ref_before(x.types.front(), y.types.front());
  });
  std::sort(report.overlaps.begin(), report.overlaps.end(), [](const TypeOverlap& x, const TypeOverlap& y) {
    if (x.jaccard != y.jaccard) {
      return x.jaccard > y.jaccard;
    }
    if (x.shared != y.shared) {
      return x.shared > y.shared;
    }
    return std::tie(x.a.label, x.b.label, x.a.source_file, x.b.source_file) <
           std::tie(y.a.label, y.b.label, y.a.source_file, y.b.source_file);
  });

  // Unreferenced: no non-structural edge into the type from anything in the
  // graph. The graph carries cross-file references (an extractor resolves a
  // type name through imports/includes, not within its own file), so a type
  // used only by the file that declares it lands here too; the caption and the
  // skill text say so.
  for (const auto& type : types) {
    if (type.incoming == 0) {
      report.unreferenced.push_back(type);
    }
  }
  std::sort(report.unreferenced.begin(), report.unreferenced.end(), [](const TypeRef& a, const TypeRef& b) {
    if (a.members.size() != b.members.size()) {
      return a.members.size() > b.members.size();
    }
    return type_ref_before(a, b);
  });

  report.total_identical = report.identical.size();
  report.total_duplicates = report.duplicates.size();
  report.total_overlaps = report.overlaps.size();
  report.total_unreferenced = report.unreferenced.size();
  return report;
}

void shed_to_budget(TypesReport& report, ReportFormat format, std::size_t budget) {
  if (budget == 0) {
    return;
  }
  const auto fits = [&](const TypesReport& candidate) {
    return estimate_report_tokens(render_types_report(candidate, format)) <= budget;
  };
  if (fits(report)) {
    return;
  }
  // One ranking across the four sections, each already best-first: a prefix
  // keeps the rows worth most and sheds from the tail of the last section.
  const auto total = report.identical.size() + report.duplicates.size() + report.overlaps.size() + report.unreferenced.size();
  const auto with_rows = [&](std::size_t keep) {
    TypesReport candidate = report;
    const auto keep_identical = std::min(keep, report.identical.size());
    keep -= keep_identical;
    const auto keep_duplicates = std::min(keep, report.duplicates.size());
    keep -= keep_duplicates;
    const auto keep_overlaps = std::min(keep, report.overlaps.size());
    keep -= keep_overlaps;
    const auto keep_unreferenced = std::min(keep, report.unreferenced.size());
    candidate.identical.resize(keep_identical);
    candidate.duplicates.resize(keep_duplicates);
    candidate.overlaps.resize(keep_overlaps);
    candidate.unreferenced.resize(keep_unreferenced);
    candidate.omitted_identical = report.total_identical - keep_identical;
    candidate.omitted_duplicates = report.total_duplicates - keep_duplicates;
    candidate.omitted_overlaps = report.total_overlaps - keep_overlaps;
    candidate.omitted_unreferenced = report.total_unreferenced - keep_unreferenced;
    return candidate;
  };
  std::size_t lo = 0;
  std::size_t hi = total;
  while (lo < hi) {
    const auto mid = (lo + hi + 1) / 2;
    if (fits(with_rows(mid))) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  report = with_rows(lo);
}

nlohmann::json types_report_json(const TypesReport& report) {
  nlohmann::json identical = nlohmann::json::array();
  for (const auto& group : report.identical) {
    nlohmann::json members = nlohmann::json::array();
    for (const auto& type : group.types) {
      members.push_back(type_ref_json(type));
    }
    identical.push_back({{"shape", group.shape}, {"types", std::move(members)}});
  }
  nlohmann::json duplicates = nlohmann::json::array();
  for (const auto& duplicate : report.duplicates) {
    nlohmann::json declarations = nlohmann::json::array();
    for (const auto& declaration : duplicate.declarations) {
      declarations.push_back(type_ref_json(declaration));
    }
    duplicates.push_back({{"label", duplicate.label},
                          {"declarations", std::move(declarations)},
                          {"min_jaccard", duplicate.min_jaccard},
                          {"max_jaccard", duplicate.max_jaccard}});
  }
  nlohmann::json overlaps = nlohmann::json::array();
  for (const auto& overlap : report.overlaps) {
    overlaps.push_back({{"a", type_ref_json(overlap.a)},
                        {"b", type_ref_json(overlap.b)},
                        {"relation", overlap.relation},
                        {"shared", overlap.shared},
                        {"jaccard", overlap.jaccard}});
  }
  nlohmann::json unreferenced = nlohmann::json::array();
  for (const auto& type : report.unreferenced) {
    unreferenced.push_back(type_ref_json(type));
  }
  return nlohmann::json{
      {"view", "types"},
      {"scope", report.scope},
      {"include_tests", report.include_tests},
      {"threshold", report.threshold},
      {"min_members", report.min_members},
      {"identical", std::move(identical)},
      {"duplicates", std::move(duplicates)},
      {"overlaps", std::move(overlaps)},
      {"unreferenced", std::move(unreferenced)},
      {"totals",
       {{"types", report.total_types},
        {"with_members", report.total_with_members},
        {"identical", report.total_identical},
        {"duplicates", report.total_duplicates},
        {"overlaps", report.total_overlaps},
        {"unreferenced", report.total_unreferenced}}},
      {"omitted",
       {{"identical", report.omitted_identical},
        {"duplicates", report.omitted_duplicates},
        {"overlaps", report.omitted_overlaps},
        {"unreferenced", report.omitted_unreferenced}}},
  };
}

std::string render_types_markdown(const TypesReport& report) {
  std::string out = "# Type definitions\n\n";
  out += types_caption(report) + " · " + plural(report.total_types, "type") + ", " +
         std::to_string(report.total_with_members) + " with members\n\n";
  out += "## Identical shapes (different names, same members)\n\n";
  if (report.identical.empty()) {
    out += "none\n";
  } else {
    out += "| members | types |\n| --- | --- |\n";
    for (const auto& group : report.identical) {
      std::string names;
      for (std::size_t i = 0; i < group.types.size(); ++i) {
        names += (i == 0 ? "`" : "<br>`") + group.types[i].label + "` " + file_line(group.types[i]);
      }
      out += "| " + join_members(group.shape) + " (" + std::to_string(group.shape.size()) + ") | " + names + " |\n";
    }
  }
  out += "\n## Duplicates (one name, several files)\n\n";
  if (report.duplicates.empty()) {
    out += "none\n";
  } else {
    out += "| type | declared in | members | overlap |\n| --- | --- | --- | --- |\n";
    for (const auto& duplicate : report.duplicates) {
      std::string where;
      std::string members;
      const auto shown = std::min(duplicate.declarations.size(), kMarkdownDeclarationCap);
      for (std::size_t i = 0; i < shown; ++i) {
        const auto& declaration = duplicate.declarations[i];
        where += (i == 0 ? "`" : "<br>`") + file_line(declaration) + "`";
        members += (i == 0 ? "" : "<br>") + (declaration.members.empty() ? std::string("(none)") : join_members(declaration.members));
      }
      if (duplicate.declarations.size() > shown) {
        where += "<br>+" + std::to_string(duplicate.declarations.size() - shown) + " more files";
      }
      const auto overlap = duplicate.min_jaccard == duplicate.max_jaccard
                               ? format_ratio(duplicate.min_jaccard)
                               : format_ratio(duplicate.min_jaccard) + "–" + format_ratio(duplicate.max_jaccard);
      out += "| `" + duplicate.label + "` (" + plural(duplicate.declarations.size(), "declaration") + ") | " + where + " | " +
             members + " | " + overlap + " |\n";
    }
  }
  out += "\n## Overlapping shapes (different names)\n\n";
  if (report.overlaps.empty()) {
    out += "none\n";
  } else {
    out += "| a | b | relation | shared | jaccard |\n| --- | --- | --- | ---: | ---: |\n";
    for (const auto& overlap : report.overlaps) {
      out += "| `" + overlap.a.label + "` " + file_line(overlap.a) + " (" + std::to_string(overlap.a.members.size()) + ") | `" +
             overlap.b.label + "` " + file_line(overlap.b) + " (" + std::to_string(overlap.b.members.size()) + ") | " +
             overlap.relation + " | " + std::to_string(overlap.shared) + " | " + format_ratio(overlap.jaccard) + " |\n";
    }
  }
  out += "\n## Unreferenced (no other symbol or file in the graph refers to them)\n\n";
  if (report.unreferenced.empty()) {
    out += "none\n";
  } else {
    out += "| type | kind | where | members |\n| --- | --- | --- | ---: |\n";
    for (const auto& type : report.unreferenced) {
      out += "| `" + type.label + "` | " + type.kind + " | " + file_line(type) + " | " + std::to_string(type.members.size()) + " |\n";
    }
  }
  out += "\n" + types_omitted_caption(report) + "\n";
  return out;
}

std::string render_types_report(const TypesReport& report, ReportFormat format) {
  switch (format) {
    case ReportFormat::Json:
      return types_report_json(report).dump();
    case ReportFormat::Mermaid:
    case ReportFormat::Svg:
    case ReportFormat::Markdown:
      return render_types_markdown(report);
  }
  return {};
}

// ---- clones view --------------------------------------------------------------

namespace {

// A shingle shared by more functions than this is boilerplate every body has
// (`return ID ;`, `if ( ID ) {`) and carries no signal about copying; its
// posting list is skipped when gathering candidate pairs. Lower would drop
// legitimately common clone stretches; higher only costs time.
constexpr std::size_t kHotShingleCap = 512;

[[nodiscard]] std::string clone_file_line(const CloneMember& member) {
  std::string out = member.source_file;
  if (member.line > 0) {
    out += ":" + std::to_string(member.line);
    if (member.end_line > member.line) {
      out += "-" + std::to_string(member.end_line);
    }
  }
  return out;
}

[[nodiscard]] std::string clones_caption(const ClonesReport& report) {
  std::string caption = report.scope.empty() ? std::string("whole project") : "scope " + report.scope;
  caption += report.include_tests ? ", tests merged" : ", test-only classes listed separately";
  caption += ", threshold " + format_ratio(report.threshold) + ", min_tokens " + std::to_string(report.min_tokens);
  return caption;
}

[[nodiscard]] std::string classes_caption(std::size_t count, const char* prefix) {
  return std::to_string(count) + " " + prefix + (count == 1 ? "class" : "classes");
}

[[nodiscard]] std::string clones_omitted_caption(const ClonesReport& report) {
  return "omitted: " + classes_caption(report.omitted_classes, "") + ", " + classes_caption(report.omitted_test_classes, "test ") +
         " (of " + classes_caption(report.total_classes, "") + ", " + classes_caption(report.total_test_classes, "test ") + ")";
}

[[nodiscard]] nlohmann::json clone_member_json(const CloneMember& member) {
  nlohmann::json out{{"id", member.id}, {"label", member.label}, {"file", member.source_file}, {"tokens", member.tokens}};
  if (member.line > 0) {
    out["line"] = member.line;
    out["end_line"] = member.end_line;
  }
  return out;
}

[[nodiscard]] nlohmann::json clone_class_json(const CloneClass& clone) {
  nlohmann::json members = nlohmann::json::array();
  for (const auto& member : clone.members) {
    members.push_back(clone_member_json(member));
  }
  return nlohmann::json{{"size", clone.members.size()}, {"similarity", clone.similarity}, {"tokens", clone.tokens},
                        {"members", std::move(members)}};
}

void render_clone_table(const std::vector<CloneClass>& classes, std::string& out) {
  if (classes.empty()) {
    out += "none\n";
    return;
  }
  out += "| copies | similarity | tokens | members |\n| ---: | ---: | ---: | --- |\n";
  for (const auto& clone : classes) {
    std::string members;
    for (std::size_t i = 0; i < clone.members.size(); ++i) {
      members += (i == 0 ? "`" : "<br>`") + clone.members[i].label + "` " + clone_file_line(clone.members[i]);
    }
    out += "| " + std::to_string(clone.members.size()) + " | " + format_ratio(clone.similarity) + " | " +
           std::to_string(clone.tokens) + " | " + members + " |\n";
  }
}

}  // namespace

ClonesReport build_clones_report(const GraphSnapshot& graph, const ReportRequest& request) {
  ClonesReport report;
  report.scope = request.scope;
  report.include_tests = request.include_tests;
  report.threshold = request.threshold;
  report.min_tokens = request.min_tokens;

  fs::path root;
  if (!request.project_root.empty()) {
    std::error_code ec;
    root = fs::weakly_canonical(request.project_root, ec);
    if (ec) {
      root = request.project_root.lexically_normal();
    }
  }

  // Candidates: function nodes in scope. Test roots stay in (they are where the
  // copies live) but are remembered so their classes can be bucketed.
  struct Candidate {
    CloneMember member;
    const FunctionFingerprint* fingerprint = nullptr;
    bool in_tests = false;
  };
  std::vector<Candidate> candidates;
  std::unordered_map<std::string, std::string> directory_of_file;
  for (const auto& node : graph.nodes) {
    if (node.kind != "function" || node.source_file.empty() || is_enrichment_node_id(node.id) || is_memory_node_id(node.id)) {
      continue;
    }
    auto dir_it = directory_of_file.find(node.source_file);
    if (dir_it == directory_of_file.end()) {
      dir_it = directory_of_file.emplace(node.source_file, module_for(node.source_file, root, 1 << 20)).first;
    }
    if (!in_scope(dir_it->second, request.scope)) {
      continue;
    }
    ++report.total_functions;
    const auto fingerprint = graph.fingerprints.find(node.id);
    if (fingerprint == graph.fingerprints.end()) {
      continue;
    }
    ++report.total_fingerprinted;
    if (fingerprint->second.tokens < report.min_tokens || fingerprint->second.shingles.empty()) {
      continue;
    }
    ++report.total_eligible;
    candidates.push_back(Candidate{
        .member = CloneMember{
            .id = node.id,
            .label = node.label,
            .source_file = relative_file(node.source_file, root),
            .line = node.source_location ? node.source_location->start_line : 0,
            .end_line = node.source_location ? node.source_location->end_line : 0,
            .tokens = fingerprint->second.tokens,
        },
        .fingerprint = &fingerprint->second,
        .in_tests = is_test_module(dir_it->second),
    });
  }
  if (report.total_fingerprinted < report.total_functions) {
    report.hint = std::to_string(report.total_functions - report.total_fingerprinted) + " of " +
                  std::to_string(report.total_functions) +
                  " functions have no fingerprint: the graph was loaded from a persist written before fingerprints "
                  "existed. Run `cgraph-client --root PATH update .` (a full rescan) to compute them.";
  }

  // Candidate pairs through an inverted index on shingle hash: a pair is
  // visited only when it shares a shingle that is not boilerplate. The Jaccard
  // is then computed exactly over both full sets -- counting only the shingles
  // that produced the candidacy under-scores a pair whenever one of its shared
  // shingles is hot (18 identical `write_file` copies scored 5/7 that way).
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> postings;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    for (const auto shingle : candidates[i].fingerprint->shingles) {
      postings[shingle].push_back(i);
    }
  }
  std::vector<std::size_t> parent(candidates.size());
  for (std::size_t i = 0; i < parent.size(); ++i) {
    parent[i] = i;
  }
  const auto find_root = [&](std::size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  // Per function: count the non-hot shingles shared with every partner, then
  // bound the pair's Jaccard from above (the partner can share at most every
  // hot shingle this function has on top of the counted ones). Only pairs whose
  // bound clears the threshold get the exact merge-walk -- on a 4,261-function
  // TypeScript app that cut the op from 8 s to under 2 s with identical classes.
  std::unordered_map<std::size_t, std::size_t> shared;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    shared.clear();
    std::size_t hot_in_i = 0;
    for (const auto shingle : candidates[i].fingerprint->shingles) {
      const auto& list = postings[shingle];
      if (list.size() > kHotShingleCap) {
        ++hot_in_i;
        continue;
      }
      for (const auto j : list) {
        if (j > i) {
          ++shared[j];
        }
      }
    }
    const auto size_i = candidates[i].fingerprint->shingles.size();
    for (const auto& [j, common] : shared) {
      const auto size_j = candidates[j].fingerprint->shingles.size();
      const auto at_most = std::min(common + hot_in_i, std::min(size_i, size_j));
      const auto smallest_union = size_i + size_j - at_most;
      const auto upper = smallest_union == 0 ? 0.0 : static_cast<double>(at_most) / static_cast<double>(smallest_union);
      if (upper < report.threshold) {
        continue;
      }
      if (fingerprint_similarity(*candidates[i].fingerprint, *candidates[j].fingerprint) >= report.threshold) {
        parent[find_root(i)] = find_root(j);
      }
    }
  }

  std::map<std::size_t, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    groups[find_root(i)].push_back(i);
  }
  for (auto& [leader, indices] : groups) {
    if (indices.size() < 2) {
      continue;
    }
    CloneClass clone;
    clone.similarity = 1.0;
    clone.tokens = candidates[indices.front()].member.tokens;
    bool all_tests = true;
    for (std::size_t x = 0; x < indices.size(); ++x) {
      const auto& candidate = candidates[indices[x]];
      clone.members.push_back(candidate.member);
      clone.tokens = std::min(clone.tokens, candidate.member.tokens);
      all_tests = all_tests && candidate.in_tests;
      for (std::size_t y = x + 1; y < indices.size(); ++y) {
        clone.similarity = std::min(clone.similarity, fingerprint_similarity(*candidate.fingerprint, *candidates[indices[y]].fingerprint));
      }
    }
    std::sort(clone.members.begin(), clone.members.end(), [](const CloneMember& a, const CloneMember& b) {
      return std::tie(a.source_file, a.line, a.label) < std::tie(b.source_file, b.line, b.label);
    });
    report.total_members += clone.members.size();
    if (all_tests && !report.include_tests) {
      report.test_classes.push_back(std::move(clone));
    } else {
      report.classes.push_back(std::move(clone));
    }
  }
  const auto by_value = [](const CloneClass& a, const CloneClass& b) {
    if (a.members.size() != b.members.size()) {
      return a.members.size() > b.members.size();
    }
    if (a.similarity != b.similarity) {
      return a.similarity > b.similarity;
    }
    if (a.tokens != b.tokens) {
      return a.tokens > b.tokens;
    }
    return std::tie(a.members.front().source_file, a.members.front().line) <
           std::tie(b.members.front().source_file, b.members.front().line);
  };
  std::sort(report.classes.begin(), report.classes.end(), by_value);
  std::sort(report.test_classes.begin(), report.test_classes.end(), by_value);
  report.total_classes = report.classes.size();
  report.total_test_classes = report.test_classes.size();
  return report;
}

void shed_to_budget(ClonesReport& report, ReportFormat format, std::size_t budget) {
  if (budget == 0) {
    return;
  }
  const auto fits = [&](const ClonesReport& candidate) {
    return estimate_report_tokens(render_clones_report(candidate, format)) <= budget;
  };
  if (fits(report)) {
    return;
  }
  const auto total = report.classes.size() + report.test_classes.size();
  const auto with_rows = [&](std::size_t keep) {
    ClonesReport candidate = report;
    const auto keep_classes = std::min(keep, report.classes.size());
    const auto keep_tests = std::min(keep - keep_classes, report.test_classes.size());
    candidate.classes.resize(keep_classes);
    candidate.test_classes.resize(keep_tests);
    candidate.omitted_classes = report.total_classes - keep_classes;
    candidate.omitted_test_classes = report.total_test_classes - keep_tests;
    return candidate;
  };
  std::size_t lo = 0;
  std::size_t hi = total;
  while (lo < hi) {
    const auto mid = (lo + hi + 1) / 2;
    if (fits(with_rows(mid))) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  report = with_rows(lo);
}

nlohmann::json clones_report_json(const ClonesReport& report) {
  nlohmann::json classes = nlohmann::json::array();
  for (const auto& clone : report.classes) {
    classes.push_back(clone_class_json(clone));
  }
  nlohmann::json test_classes = nlohmann::json::array();
  for (const auto& clone : report.test_classes) {
    test_classes.push_back(clone_class_json(clone));
  }
  nlohmann::json out{
      {"view", "clones"},
      {"scope", report.scope},
      {"include_tests", report.include_tests},
      {"threshold", report.threshold},
      {"min_tokens", report.min_tokens},
      {"classes", std::move(classes)},
      {"test_classes", std::move(test_classes)},
      {"totals",
       {{"functions", report.total_functions},
        {"fingerprinted", report.total_fingerprinted},
        {"eligible", report.total_eligible},
        {"classes", report.total_classes},
        {"test_classes", report.total_test_classes},
        {"members", report.total_members}}},
      {"omitted", {{"classes", report.omitted_classes}, {"test_classes", report.omitted_test_classes}}},
  };
  if (!report.hint.empty()) {
    out["hint"] = report.hint;
  }
  return out;
}

std::string render_clones_markdown(const ClonesReport& report) {
  std::string out = "# Function clones\n\n";
  out += clones_caption(report) + " · " + plural(report.total_functions, "function") + ", " +
         std::to_string(report.total_fingerprinted) + " fingerprinted, " + std::to_string(report.total_eligible) +
         " at or above the token floor, " + std::to_string(report.total_members) + " in a clone class\n\n";
  if (!report.hint.empty()) {
    out += "> " + report.hint + "\n\n";
  }
  out += "## Clone classes\n\n";
  render_clone_table(report.classes, out);
  if (!report.include_tests) {
    out += "\n## Test-only clone classes\n\n";
    render_clone_table(report.test_classes, out);
  }
  out += "\n" + clones_omitted_caption(report) + "\n";
  return out;
}

std::string render_clones_report(const ClonesReport& report, ReportFormat format) {
  switch (format) {
    case ReportFormat::Json:
      return clones_report_json(report).dump();
    case ReportFormat::Mermaid:
    case ReportFormat::Svg:
    case ReportFormat::Markdown:
      return render_clones_markdown(report);
  }
  return {};
}

// ---- design view --------------------------------------------------------------

namespace {

// Children drawn per flow node; the rest are counted in `more`. Four keeps a
// three-hop flow under a screen while showing where the weight goes.
constexpr std::size_t kFlowBranch = 4;
constexpr std::size_t kUnreachedSamples = 5;
constexpr std::array<std::string_view, 8> kHttpVerbs = {"get", "post", "put", "patch", "delete", "head", "options", "all"};

[[nodiscard]] bool is_call_relation(std::string_view relation) {
  return relation == "CALLS" || relation == "dispatches_to";
}

[[nodiscard]] bool is_main_label(std::string_view label) {
  return label == "main" || label == "Main" || label == "__main__";
}

// `notebookRoutes.get /starred-notes` or `get /health`: the inline HTTP route
// handlers the JavaScript extractor names (CGR-4 follow-up).
[[nodiscard]] bool is_route_label(std::string_view label) {
  const auto space = label.find(' ');
  if (space == std::string_view::npos || space + 1 >= label.size() || label[space + 1] != '/') {
    return false;
  }
  auto head = label.substr(0, space);
  if (const auto dot = head.rfind('.'); dot != std::string_view::npos) {
    head = head.substr(dot + 1);
  }
  return std::find(kHttpVerbs.begin(), kHttpVerbs.end(), head) != kHttpVerbs.end();
}

[[nodiscard]] std::string file_stem(const std::string& source_file) {
  return fs::path(source_file).stem().generic_string();
}

[[nodiscard]] bool under_directory(const std::string& source_file, std::string_view name) {
  for (const auto& part : fs::path(source_file).parent_path()) {
    if (part.generic_string() == name) {
      return true;
    }
  }
  return false;
}

// Next.js / Remix-style framework entries: the file is the route. A function
// in `app/**/page.tsx`, `layout.tsx`, `template.tsx`, `loading.tsx`,
// `error.tsx`, `not-found.tsx` or `pages/**/*.tsx` is a page; an exported
// `GET`/`POST`/... in `app/**/route.ts` is a route.
[[nodiscard]] bool is_page_file(const std::string& source_file) {
  static constexpr std::array<std::string_view, 6> kPageStems = {"page", "layout", "template", "loading", "error", "not-found"};
  const auto stem = file_stem(source_file);
  if (under_directory(source_file, "app") &&
      std::find(kPageStems.begin(), kPageStems.end(), std::string_view(stem)) != kPageStems.end()) {
    return true;
  }
  return under_directory(source_file, "pages") && !under_directory(source_file, "api");
}

[[nodiscard]] bool is_route_file_handler(const std::string& source_file, std::string_view label) {
  if (!next_route_path(source_file)) {
    return false;
  }
  std::string lower(label);
  for (auto& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return std::find(kHttpVerbs.begin(), kHttpVerbs.end(), std::string_view(lower)) != kHttpVerbs.end();
}

[[nodiscard]] std::string flow_where(const FlowNode& node) {
  return node.source_file + (node.line > 0 ? ":" + std::to_string(node.line) : std::string{});
}

[[nodiscard]] std::string design_caption(const DesignReport& report) {
  std::string caption = report.scope.empty() ? std::string("whole project") : "scope " + report.scope;
  caption += report.include_tests ? ", tests included" : ", tests excluded";
  caption += ", " + std::to_string(report.hops) + (report.hops == 1 ? " hop" : " hops");
  return caption;
}

[[nodiscard]] std::string design_omitted_caption(const DesignReport& report) {
  return "omitted: " + std::to_string(report.omitted_entries) + " of " + plural(report.total_entries, "entry point");
}

nlohmann::json flow_json(const FlowNode& node) {
  nlohmann::json out{{"id", node.id}, {"label", node.label}, {"file", node.source_file}, {"reach", node.reach}};
  if (node.line > 0) {
    out["line"] = node.line;
  }
  nlohmann::json children = nlohmann::json::array();
  for (const auto& child : node.children) {
    children.push_back(flow_json(child));
  }
  out["children"] = std::move(children);
  if (node.more > 0) {
    out["more"] = node.more;
  }
  return out;
}

void render_flow_markdown(const FlowNode& node, int depth, std::string& out) {
  for (const auto& child : node.children) {
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + "- `" + child.label + "` " + flow_where(child);
    if (child.reach > 0) {
      out += " (reach " + std::to_string(child.reach) + ")";
    }
    out += "\n";
    render_flow_markdown(child, depth + 1, out);
  }
  if (node.more > 0) {
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + "- +" + std::to_string(node.more) +
           (node.more == 1 ? " more callee\n" : " more callees\n");
  }
}

}  // namespace

DesignReport build_design_report(const GraphSnapshot& graph, const ReportRequest& request) {
  DesignReport report;
  report.scope = request.scope;
  report.include_tests = request.include_tests;
  report.hops = std::max(1, request.hops);

  fs::path root;
  if (!request.project_root.empty()) {
    std::error_code ec;
    root = fs::weakly_canonical(request.project_root, ec);
    if (ec) {
      root = request.project_root.lexically_normal();
    }
  }

  // Functions in scope, outside test roots unless asked. Indexed for the walks.
  struct Function {
    const Node* node = nullptr;
    std::string module;
    std::string relative;
    std::vector<std::size_t> callees;
    std::size_t callers = 0;
  };
  std::vector<Function> functions;
  std::unordered_map<std::string, std::size_t> index_of;
  std::unordered_map<std::string, std::string> directory_of_file;
  for (const auto& node : graph.nodes) {
    if (node.kind != "function" || node.source_file.empty() || is_enrichment_node_id(node.id) || is_memory_node_id(node.id)) {
      continue;
    }
    auto dir_it = directory_of_file.find(node.source_file);
    if (dir_it == directory_of_file.end()) {
      dir_it = directory_of_file.emplace(node.source_file, module_for(node.source_file, root, 1 << 20)).first;
    }
    if (!is_source_module(dir_it->second, request)) {
      continue;
    }
    index_of.emplace(node.id, functions.size());
    functions.push_back(Function{
        .node = &node,
        .module = module_for(node.source_file, root, kDefaultModuleDepth),
        .relative = relative_file(node.source_file, root),
    });
  }
  report.total_functions = functions.size();
  std::set<std::pair<std::size_t, std::size_t>> seen_edges;
  for (const auto& edge : graph.edges) {
    if (!is_call_relation(edge.relation)) {
      continue;
    }
    const auto from = index_of.find(edge.source);
    const auto to = index_of.find(edge.target);
    if (from == index_of.end() || to == index_of.end() || from->second == to->second) {
      continue;
    }
    if (seen_edges.emplace(from->second, to->second).second) {
      functions[from->second].callees.push_back(to->second);
      ++functions[to->second].callers;
    }
  }
  for (auto& function : functions) {
    std::sort(function.callees.begin(), function.callees.end(), [&](std::size_t a, std::size_t b) {
      return functions[a].node->label < functions[b].node->label;
    });
  }

  // Unbounded reach per function, memoized through a BFS each; graphs here are
  // a few thousand functions, and the walk is over the call adjacency only.
  std::vector<std::size_t> reach(functions.size(), 0);
  {
    std::vector<std::size_t> mark(functions.size(), static_cast<std::size_t>(-1));
    std::vector<std::size_t> queue;
    for (std::size_t start = 0; start < functions.size(); ++start) {
      queue.clear();
      queue.push_back(start);
      mark[start] = start;
      std::size_t count = 0;
      for (std::size_t head = 0; head < queue.size(); ++head) {
        for (const auto next : functions[queue[head]].callees) {
          if (mark[next] != start) {
            mark[next] = start;
            queue.push_back(next);
            ++count;
          }
        }
      }
      reach[start] = count;
    }
  }

  // Entry points, most specific kind first.
  std::vector<std::size_t> entry_indices;
  std::vector<std::string> entry_kinds(functions.size());
  for (std::size_t i = 0; i < functions.size(); ++i) {
    const auto& function = functions[i];
    const auto& label = function.node->label;
    std::string kind;
    if (is_main_label(label)) {
      kind = "main";
    } else if (is_route_label(label) || is_route_file_handler(function.node->source_file, label)) {
      kind = "route";
    } else if (is_page_file(function.node->source_file) && function.callers == 0) {
      kind = "page";
    } else if (function.callers == 0 && !function.callees.empty()) {
      kind = "root";
    } else {
      continue;
    }
    entry_kinds[i] = kind;
    entry_indices.push_back(i);
    ++report.by_kind[kind];
  }
  std::sort(entry_indices.begin(), entry_indices.end(), [&](std::size_t a, std::size_t b) {
    if (reach[a] != reach[b]) {
      return reach[a] > reach[b];
    }
    if (functions[a].callees.size() != functions[b].callees.size()) {
      return functions[a].callees.size() > functions[b].callees.size();
    }
    return std::tie(functions[a].node->label, functions[a].relative) < std::tie(functions[b].node->label, functions[b].relative);
  });

  // Layers: shortest call distance from any entry point.
  std::vector<int> depth(functions.size(), -1);
  {
    std::vector<std::size_t> queue;
    for (const auto i : entry_indices) {
      depth[i] = 0;
      queue.push_back(i);
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
      for (const auto next : functions[queue[head]].callees) {
        if (depth[next] < 0) {
          depth[next] = depth[queue[head]] + 1;
          queue.push_back(next);
        }
      }
    }
  }
  std::map<int, std::map<std::string, std::size_t>> layer_modules;
  std::vector<std::size_t> unreached;
  for (std::size_t i = 0; i < functions.size(); ++i) {
    if (depth[i] < 0) {
      unreached.push_back(i);
      continue;
    }
    ++layer_modules[depth[i]][functions[i].module];
  }
  for (const auto& [d, modules] : layer_modules) {
    DesignLayer layer;
    layer.depth = d;
    std::vector<std::pair<std::string, std::size_t>> ranked(modules.begin(), modules.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (const auto& [name, count] : ranked) {
      layer.functions += count;
      if (layer.modules.size() < 3) {
        layer.modules.push_back(name);
      }
    }
    report.layers.push_back(std::move(layer));
  }
  report.total_entries = entry_indices.size();
  report.total_reached = static_cast<std::size_t>(std::count_if(depth.begin(), depth.end(), [](int d) { return d > 0; }));
  report.total_unreached = unreached.size();
  std::sort(unreached.begin(), unreached.end(), [&](std::size_t a, std::size_t b) {
    if (functions[a].callers != functions[b].callers) {
      return functions[a].callers > functions[b].callers;
    }
    return functions[a].node->label < functions[b].node->label;
  });
  for (const auto i : unreached) {
    if (report.unreached_samples.size() >= kUnreachedSamples) {
      break;
    }
    // Distinct labels: a class's constructor, destructor and method share one.
    const auto& label = functions[i].node->label;
    if (std::find(report.unreached_samples.begin(), report.unreached_samples.end(), label) == report.unreached_samples.end()) {
      report.unreached_samples.push_back(label);
    }
  }

  // Flows: a tree to `hops` from each entry, children by reach, each function
  // drawn once per flow so a cycle ends where it re-enters.
  const auto make_node = [&](std::size_t i) {
    return FlowNode{
        .id = functions[i].node->id,
        .label = functions[i].node->label,
        .source_file = functions[i].relative,
        .line = functions[i].node->source_location ? functions[i].node->source_location->start_line : 0,
        .reach = reach[i],
    };
  };
  std::function<void(FlowNode&, std::size_t, int, std::set<std::size_t>&)> expand;
  expand = [&](FlowNode& node, std::size_t i, int remaining, std::set<std::size_t>& drawn) {
    if (remaining == 0) {
      return;
    }
    std::vector<std::size_t> callees;
    for (const auto c : functions[i].callees) {
      if (drawn.insert(c).second) {
        callees.push_back(c);
      }
    }
    std::sort(callees.begin(), callees.end(), [&](std::size_t a, std::size_t b) {
      return reach[a] != reach[b] ? reach[a] > reach[b] : functions[a].node->label < functions[b].node->label;
    });
    if (callees.size() > kFlowBranch) {
      node.more = callees.size() - kFlowBranch;
      callees.resize(kFlowBranch);
    }
    for (const auto c : callees) {
      node.children.push_back(make_node(c));
      expand(node.children.back(), c, remaining - 1, drawn);
    }
  };
  for (const auto i : entry_indices) {
    DesignEntry entry;
    entry.id = functions[i].node->id;
    entry.label = functions[i].node->label;
    entry.kind = entry_kinds[i];
    entry.source_file = functions[i].relative;
    entry.line = functions[i].node->source_location ? functions[i].node->source_location->start_line : 0;
    entry.module = functions[i].module;
    entry.fan_out = functions[i].callees.size();
    entry.reach = reach[i];
    entry.flow = make_node(i);
    std::set<std::size_t> drawn{i};
    expand(entry.flow, i, report.hops, drawn);
    report.entries.push_back(std::move(entry));
  }
  return report;
}

void shed_to_budget(DesignReport& report, ReportFormat format, std::size_t budget) {
  if (budget == 0) {
    return;
  }
  const auto fits = [&](const DesignReport& candidate) {
    return estimate_report_tokens(render_design_report(candidate, format)) <= budget;
  };
  if (fits(report)) {
    return;
  }
  const auto with_rows = [&](std::size_t keep) {
    DesignReport candidate = report;
    candidate.entries.resize(std::min(keep, report.entries.size()));
    candidate.omitted_entries = report.total_entries - candidate.entries.size();
    return candidate;
  };
  std::size_t lo = 0;
  std::size_t hi = report.entries.size();
  while (lo < hi) {
    const auto mid = (lo + hi + 1) / 2;
    if (fits(with_rows(mid))) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  report = with_rows(lo);
}

nlohmann::json design_report_json(const DesignReport& report) {
  nlohmann::json entries = nlohmann::json::array();
  for (const auto& entry : report.entries) {
    nlohmann::json row{{"id", entry.id}, {"label", entry.label}, {"kind", entry.kind}, {"file", entry.source_file},
                       {"module", entry.module}, {"fan_out", entry.fan_out}, {"reach", entry.reach},
                       {"flow", flow_json(entry.flow)}};
    if (entry.line > 0) {
      row["line"] = entry.line;
    }
    entries.push_back(std::move(row));
  }
  nlohmann::json layers = nlohmann::json::array();
  for (const auto& layer : report.layers) {
    layers.push_back({{"depth", layer.depth}, {"functions", layer.functions}, {"modules", layer.modules}});
  }
  return nlohmann::json{
      {"view", "design"},
      {"scope", report.scope},
      {"include_tests", report.include_tests},
      {"hops", report.hops},
      {"entry_points", std::move(entries)},
      {"layers", std::move(layers)},
      {"unreached_samples", report.unreached_samples},
      {"totals",
       {{"functions", report.total_functions},
        {"entry_points", report.total_entries},
        {"by_kind", report.by_kind},
        {"reached", report.total_reached},
        {"unreached", report.total_unreached}}},
      {"omitted", {{"entry_points", report.omitted_entries}}},
  };
}

std::string render_design_mermaid(const DesignReport& report) {
  std::string out = "flowchart TD\n";
  out += "  %% design: " + design_caption(report) + "; " + plural(report.total_entries, "entry point") + "\n";
  out += "  classDef entry fill:#eef2ff,stroke:#4f46e5,stroke-width:2px\n";
  std::map<std::string, std::string> ids;
  std::set<std::pair<std::string, std::string>> edges;
  const auto id_for = [&](const FlowNode& node) {
    auto it = ids.find(node.id);
    if (it == ids.end()) {
      const auto mermaid_id = "f" + std::to_string(ids.size());
      it = ids.emplace(node.id, mermaid_id).first;
      out += "  " + mermaid_id + "[\"" + mermaid_label(node.label) + "\"]\n";
    }
    return it->second;
  };
  std::function<void(const FlowNode&)> walk = [&](const FlowNode& node) {
    const auto from = id_for(node);
    for (const auto& child : node.children) {
      const auto to = id_for(child);
      if (edges.emplace(from, to).second) {
        out += "  " + from + " --> " + to + "\n";
      }
      walk(child);
    }
    if (node.more > 0) {
      const auto more_id = from + "_more";
      out += "  " + more_id + "([\"+" + std::to_string(node.more) + " more\"])\n";
      out += "  " + from + " -.-> " + more_id + "\n";
    }
  };
  for (const auto& entry : report.entries) {
    out += "  %% " + entry.kind + ": " + entry.label + " (" + entry.source_file + ", reach " + std::to_string(entry.reach) + ")\n";
    walk(entry.flow);
    out += "  class " + id_for(entry.flow) + " entry\n";
  }
  out += "  %% " + design_omitted_caption(report) + "\n";
  return out;
}

std::string render_design_markdown(const DesignReport& report) {
  std::string out = "# Program design\n\n";
  out += design_caption(report) + " · " + plural(report.total_functions, "function") + ", " +
         plural(report.total_entries, "entry point") + ", " + std::to_string(report.total_reached) + " reached, " +
         std::to_string(report.total_unreached) + " unreached\n\n";
  out += "## Entry points\n\n";
  if (report.entries.empty()) {
    out += "none\n";
  } else {
    out += "| kind | entry | where | module | calls | reach |\n| --- | --- | --- | --- | ---: | ---: |\n";
    for (const auto& entry : report.entries) {
      out += "| " + entry.kind + " | `" + entry.label + "` | " + flow_where(entry.flow) + " | " + entry.module + " | " +
             std::to_string(entry.fan_out) + " | " + std::to_string(entry.reach) + " |\n";
    }
  }
  out += "\n## Call flows\n\n";
  for (const auto& entry : report.entries) {
    out += "### " + entry.kind + ": `" + entry.label + "` (" + flow_where(entry.flow) + ")\n\n";
    if (entry.flow.children.empty()) {
      out += "- (calls nothing the graph resolves)\n";
    } else {
      render_flow_markdown(entry.flow, 0, out);
    }
    out += "\n";
  }
  out += "## Layers (shortest call distance from an entry point)\n\n";
  if (report.layers.empty()) {
    out += "none\n";
  } else {
    out += "| depth | functions | mostly in |\n| ---: | ---: | --- |\n";
    for (const auto& layer : report.layers) {
      std::string modules;
      for (std::size_t i = 0; i < layer.modules.size(); ++i) {
        modules += (i == 0 ? "`" : ", `") + layer.modules[i] + "`";
      }
      out += "| " + std::to_string(layer.depth) + " | " + std::to_string(layer.functions) + " | " + modules + " |\n";
    }
  }
  out += "\n" + std::to_string(report.total_unreached) + " unreached from any entry point";
  if (!report.unreached_samples.empty()) {
    out += " (most called first):";
    for (std::size_t i = 0; i < report.unreached_samples.size(); ++i) {
      out += (i == 0 ? " `" : ", `") + report.unreached_samples[i] + "`";
    }
  }
  out += "\n\n" + design_omitted_caption(report) + "\n";
  return out;
}

std::string render_design_report(const DesignReport& report, ReportFormat format) {
  switch (format) {
    case ReportFormat::Json:
      return design_report_json(report).dump();
    case ReportFormat::Mermaid:
    case ReportFormat::Svg:
      return render_design_mermaid(report);
    case ReportFormat::Markdown:
      return render_design_markdown(report);
  }
  return {};
}

std::string render_modules_report(const ModulesReport& report, ReportFormat format) {
  switch (format) {
    case ReportFormat::Json:
      return modules_report_json(report).dump();
    case ReportFormat::Mermaid:
      return render_modules_mermaid(report);
    case ReportFormat::Svg:
      return render_modules_svg(report);
    case ReportFormat::Markdown:
      return render_modules_markdown(report);
  }
  return {};
}

nlohmann::json report_response(const GraphSnapshot& graph, const nlohmann::json& params,
                               const std::filesystem::path& project_root) {
  ReportRequest request;
  request.project_root = project_root;
  if (const auto error = parse_report_request(params, request)) {
    return nlohmann::json{{"ok", false}, {"error", *error}};
  }
  nlohmann::json result;
  std::string rendered;
  const bool tabular = request.view == ReportView::Types || request.view == ReportView::Clones;
  if (tabular && (request.format == ReportFormat::Mermaid || request.format == ReportFormat::Svg)) {
    return nlohmann::json{
        {"ok", false},
        {"error", std::string{"report view '"} + report_view_name(request.view) + "' renders json or markdown; '" +
                      report_format_name(request.format) + "' is a diagram format for the modules view"},
        {"code", "report_format_unsupported"},
    };
  }
  if (request.view == ReportView::Design && request.format == ReportFormat::Svg) {
    return nlohmann::json{
        {"ok", false},
        {"error", "report view 'design' renders json, mermaid or markdown; svg is drawn for the modules view only"},
        {"code", "report_format_unsupported"},
    };
  }
  if (request.view == ReportView::Design) {
    auto report = build_design_report(graph, request);
    shed_to_budget(report, request.format, request.budget);
    rendered = render_design_report(report, request.format);
    result = design_report_json(report);
    if (request.format != ReportFormat::Json) {
      result.erase("entry_points");
      result.erase("layers");
      result.erase("unreached_samples");
      result["rendered"] = rendered;
    }
  } else if (request.view == ReportView::Clones) {
    auto report = build_clones_report(graph, request);
    shed_to_budget(report, request.format, request.budget);
    rendered = render_clones_report(report, request.format);
    result = clones_report_json(report);
    if (request.format == ReportFormat::Markdown) {
      result.erase("classes");
      result.erase("test_classes");
      result["rendered"] = rendered;
    }
  } else if (request.view == ReportView::Types) {
    auto report = build_types_report(graph, request);
    shed_to_budget(report, request.format, request.budget);
    rendered = render_types_report(report, request.format);
    result = types_report_json(report);
    if (request.format == ReportFormat::Markdown) {
      for (const char* section : {"identical", "duplicates", "overlaps", "unreferenced"}) {
        result.erase(section);
      }
      result["rendered"] = rendered;
    }
  } else {
    auto report = build_modules_report(graph, request);
    shed_to_budget(report, request.format, request.budget);
    rendered = render_modules_report(report, request.format);
    if (request.format == ReportFormat::Json) {
      result = modules_report_json(report);
    } else {
      result = nlohmann::json{
          {"view", "modules"},
          {"depth", report.depth},
          {"scope", report.scope},
          {"include_tests", report.include_tests},
          {"rendered", rendered},
          {"totals", {{"modules", report.total_modules}, {"edges", report.total_edges}}},
          {"omitted", {{"modules", report.omitted_modules}, {"edges", report.omitted_edges}}},
      };
    }
  }
  result["format"] = report_format_name(request.format);
  result["budget"] = request.budget;
  result["estimated_tokens"] = estimate_report_tokens(rendered);
  return nlohmann::json{{"ok", true}, {"result", std::move(result)}};
}

std::optional<std::string> report_upgrade_hint(const nlohmann::json& daemon_response) {
  if (daemon_response.value("ok", false)) {
    return std::nullopt;
  }
  const auto error = daemon_response.value("error", std::string{});
  if (!error.starts_with("unknown op")) {
    return std::nullopt;
  }
  return "the running graphd predates the report op; upgrade the daemon: stop it with "
         "`cgraph-client --root PATH shutdown` so the newer binary respawns it, then retry";
}

}  // namespace cgraph
