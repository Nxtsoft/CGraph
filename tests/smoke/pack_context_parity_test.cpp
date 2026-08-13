// Parity gate for the knapsack packing path in pack_context.
//
// Reproduces the offline harness (research/2510.00446) against a COMMITTED fixture
// pair (tests/fixtures/pack_context_parity/{graph.json,queries.jsonl}): inject each
// row's grade-2 focal seed, run the C++ `context` op with packing=knapsack at k=3,
// and compare mean packed grade-2 recall to the harness numbers.
//
// The engine weights the knapsack by char/4 over the capped source slice (step-A
// "model 4") as its RANKING heuristic; since openspec/changes/honest-context-budget
// the emitted response is additionally shed to a measured serialized ceiling, so
// the gate pins the honest packer's own measured baselines (see the Target table)
// rather than the historical over-packing harness numbers.
//
// The fixture is a deterministic, code-only graph (no research/ or build/ nodes)
// committed alongside a verbatim eval snapshot, so the gate is reproducible and
// immune to working-tree / daemon drift, and runs on every checkout including CI.
// It does NOT read the mutable cgraph-out/graph.json. The absent-artifact skip
// remains only as a defensive fallback if the fixture is somehow missing.

#include "cgraph/daemon_lifecycle.hpp"
#include "cgraph/daemon_ops.hpp"
#include "cgraph/protocol.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

double centrality_of(const cgraph::Node& node) {
  const auto it = node.properties.find("degree_centrality");
  if (it == node.properties.end()) {
    return 0.0;
  }
  try {
    return std::stod(it->second);
  } catch (const std::exception&) {
    return 0.0;
  }
}

}  // namespace

int main() {
  // Committed fixture pair (deterministic code-only graph + verbatim eval snapshot),
  // NOT the mutable cgraph-out/graph.json -- so the gate is drift-immune and runs in CI.
  const fs::path fixture = CGRAPH_PARITY_FIXTURE_DIR;
  const fs::path graph_path = fixture / "graph.json";
  const fs::path eval_path = fixture / "queries.jsonl";

  if (!fs::exists(graph_path) || !fs::exists(eval_path)) {
    std::cout << "SKIP: parity fixture absent (" << graph_path << " / " << eval_path
              << "). Regenerate per openspec stabilize-parity-gate-fixture.\n";
    return 0;  // Defensive fallback; the fixture is committed, so this should not trigger.
  }

  cgraph::DaemonState state;
  if (!cgraph::load_graph_snapshot(state, graph_path)) {
    std::cerr << "FAIL: could not load " << graph_path << "\n";
    return 1;
  }
  auto portable_graph = *cgraph::read_graph_snapshot(state);
  const fs::path repo_root = CGRAPH_REPO_ROOT;
  for (auto& node : portable_graph.nodes) {
    if (node.source_file.empty()) {
      continue;
    }
    const fs::path source_file = node.source_file;
    if (source_file.is_absolute()) {
      std::cerr << "FAIL: parity fixture source paths must be repository-relative: " << source_file << "\n";
      return 1;
    }
    node.source_file = (repo_root / source_file).lexically_normal().string();
  }
  cgraph::publish_graph_snapshot(state, std::move(portable_graph));
  const auto snapshot = cgraph::read_graph_snapshot(state);

  std::unordered_map<std::string, const cgraph::Node*> by_id;
  for (const auto& node : snapshot->nodes) {
    by_id[node.id] = &node;
  }

  // Parse symbol-granularity eval rows; focal = highest-centrality grade-2 symbol
  // present in the graph (ties by id) -- mirrors research/metric.py::resolve_focal.
  struct Row {
    std::string focal;
    std::string query;  // needed for the adaptive gather gate (query-term overlap)
    std::set<std::string> grade2;
  };
  std::vector<Row> rows;
  std::ifstream eval_file(eval_path);
  std::string line;
  while (std::getline(eval_file, line)) {
    if (line.empty()) {
      continue;
    }
    const auto row = nlohmann::json::parse(line, nullptr, false);
    if (row.is_discarded() || row.value("granularity", std::string{"symbol"}) != "symbol") {
      continue;
    }
    std::set<std::string> grade2;
    for (const auto& rel : row.value("relevant", nlohmann::json::array())) {
      if (rel.value("grade", 0) == 2) {
        grade2.insert(rel.value("node_id", std::string{}));
      }
    }
    const cgraph::Node* focal = nullptr;
    for (const auto& id : grade2) {
      const auto it = by_id.find(id);
      if (it == by_id.end()) {
        continue;
      }
      const auto* node = it->second;
      if (focal == nullptr || centrality_of(*node) > centrality_of(*focal) ||
          (centrality_of(*node) == centrality_of(*focal) && node->id > focal->id)) {
        focal = node;
      }
    }
    if (focal != nullptr) {
      rows.push_back({focal->id, row.value("query", std::string{}), std::move(grade2)});
    }
  }

  const auto mean_recall = [&](const std::string& packing, int budget) {
    double sum = 0.0;
    int n = 0;
    for (const auto& row : rows) {
      // Pin gather=fixed: the default is now adaptive (which forces knapsack), so
      // the greedy-vs-knapsack comparison must opt into fixed to isolate packing.
      const nlohmann::json params{
          {"id", row.focal}, {"budget", budget}, {"packing", packing}, {"max_depth", 3}, {"gather", "fixed"}};
      const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("context", params));
      const auto& result = response["result"];
      std::set<std::string> selected;
      if (result.contains("focus") && result["focus"].is_object()) {
        selected.insert(result["focus"].value("id", std::string{}));
      }
      for (const auto& entry : result.value("included", nlohmann::json::array())) {
        selected.insert(entry.value("id", std::string{}));
      }
      std::size_t hit = 0;
      for (const auto& id : row.grade2) {
        if (selected.count(id) != 0) {
          ++hit;
        }
      }
      if (!row.grade2.empty()) {
        sum += static_cast<double>(hit) / static_cast<double>(row.grade2.size());
        ++n;
      }
    }
    return n != 0 ? sum / static_cast<double>(n) : 0.0;
  };

  struct Target {
    int budget;
    double greedy_baseline;    // measured, honest packer (see comment below)
    double knapsack_baseline;  // measured, honest packer
    bool gated;                // 8k is neutral by design (packing ~moot once everything fits)
  };
  // Re-pinned for openspec/changes/honest-context-budget: the previous targets
  // (0.591/0.625/0.666, "model-4") were measured against a knapsack that shipped
  // ~6x its stated budget while reporting a near-perfect fit, and the old
  // one-sided knapsack>=greedy clause was satisfiable only by that overshoot.
  // These baselines are the honest packer's (both modes shed to a measured
  // serialized ceiling; knapsack sheds by value density), measured on the frozen
  // fixture at the commit introducing the ceiling. 6000 is the shipped default
  // budget. The gate is non-regression per packer plus a symmetric packing-
  // parity band -- neither packer may silently pull ahead or fall behind.
  const std::vector<Target> targets = {{2000, 0.442382, 0.430765, true},
                                       {4000, 0.515687, 0.531519, true},
                                       {6000, 0.574093, 0.586637, true},
                                       {8000, 0.608445, 0.606290, false}};
  constexpr double kTol = 0.03;

  std::cout << "pack_context knapsack parity  (N=" << rows.size() << " symbol rows, k=3)\n";
  std::cout << "budget    greedy   knapsack   |delta|   gate\n";
  int failures = 0;
  for (const auto& t : targets) {
    const double greedy = mean_recall("greedy", t.budget);
    const double knapsack = mean_recall("knapsack", t.budget);
    const double delta = std::fabs(knapsack - greedy);
    bool ok = true;
    if (t.gated) {
      ok = greedy + 1e-9 >= t.greedy_baseline - kTol &&
           knapsack + 1e-9 >= t.knapsack_baseline - kTol &&
           delta <= kTol;  // non-regression per packer AND two-sided parity
    }
    if (!ok) {
      ++failures;
    }
    std::cout << "  " << t.budget << "    " << greedy << "    " << knapsack << "    "
              << delta << "    " << (t.gated ? (ok ? "PASS" : "FAIL") : "neutral") << "\n";
  }

  // --- Adaptive gather revalidation (in-engine, this graph) -------------------
  // The Python harness (research/recall_lever.py) showed adaptive gather beats the
  // k=2 greedy baseline at a fraction of k=3's candidate cost. That result is
  // EVIDENCE; this block is the in-engine revalidation under the engine's own
  // accounting. Adaptive must (a) beat the true greedy@k=2 baseline materially and
  // (b) stay at/below full k=3 recall while gathering far fewer candidates.
  const auto measure = [&](const std::string& packing, const std::string& gather, int max_depth,
                           int budget) -> std::pair<double, double> {
    double rsum = 0.0;
    double csum = 0.0;
    int n = 0;
    for (const auto& row : rows) {
      nlohmann::json params{{"id", row.focal}, {"budget", budget}, {"packing", packing},
                            {"max_depth", max_depth}, {"gather", gather}};
      if (gather == "adaptive") {
        params["gather_theta"] = 0.05;
        params["q"] = row.query;  // the gate needs the query terms
      }
      const auto result = cgraph::handle_daemon_request(state, cgraph::make_request("context", params))["result"];
      std::set<std::string> selected;
      if (result.contains("focus") && result["focus"].is_object()) {
        selected.insert(result["focus"].value("id", std::string{}));
      }
      const auto included = result.value("included", nlohmann::json::array());
      for (const auto& entry : included) {
        selected.insert(entry.value("id", std::string{}));
      }
      const double cand = static_cast<double>(included.size()) + result.value("omitted", 0);
      std::size_t hit = 0;
      for (const auto& id : row.grade2) {
        if (selected.count(id) != 0) {
          ++hit;
        }
      }
      if (!row.grade2.empty()) {
        rsum += static_cast<double>(hit) / static_cast<double>(row.grade2.size());
        csum += cand;
        ++n;
      }
    }
    return {n != 0 ? rsum / n : 0.0, n != 0 ? csum / n : 0.0};
  };

  std::cout << "\nadaptive gather revalidation (in-engine, N=" << rows.size() << ")\n";
  std::cout << "budget   greedy@k2   adaptive   knap@k3   d(adp-k2)   cand k2/adp/k3   gate\n";
  // Honest-ceiling re-pin: with the budget enforced, small bundles shrink for
  // every gather mode and adaptive's 2k gain over greedy@k2 compressed from
  // +0.117 (measured under the over-packing packer) to +0.0146. The candidate-
  // pool advantage is unchanged (24.6 vs 41.97 at k3). Entry costs include the
  // absolute source path, so measured gains vary with checkout depth: the 4k
  // gain measured +0.0387 in a deep local worktree and +0.0272 on CI's short
  // runner root. Floors sit below every observed environment while still
  // requiring a material positive gain.
  const std::map<int, double> kMinAdaptiveGain{{2000, 0.005}, {4000, 0.020}};
  for (const int budget : {2000, 4000}) {  // 8k neutral: the ego graph mostly fits
    const auto [r_k2, c_k2] = measure("greedy", "fixed", 2, budget);
    const auto [r_adp, c_adp] = measure("knapsack", "adaptive", 3, budget);
    const auto [r_k3, c_k3] = measure("knapsack", "fixed", 3, budget);
    const double delta = r_adp - r_k2;
    // Positive gain over the true baseline and a strictly smaller candidate pool
    // than k3 (the recall/cost win, in-engine). The old "no better than k3+0.02"
    // cap assumed an unenforced budget, where the k3 superset could only help;
    // with the measured ceiling a smaller, better-ranked pool legitimately packs
    // more relevant entries than the superset, so the cap is gone.
    const bool ok = delta >= kMinAdaptiveGain.at(budget) && c_adp < c_k3;
    if (!ok) {
      ++failures;
    }
    std::cout << "  " << budget << "   " << r_k2 << "   " << r_adp << "   " << r_k3 << "   " << delta << "   "
              << c_k2 << "/" << c_adp << "/" << c_k3 << "   " << (ok ? "PASS" : "FAIL") << "\n";
  }

  if (failures != 0) {
    std::cerr << failures << " parity assertion(s) failed\n";
    return 1;
  }
  std::cout << "parity OK\n";
  return 0;
}
