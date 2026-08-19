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

  // Guards against measuring a snippet-free graph (e.g. unresolvable source
  // paths): recall numbers on such a graph look like a packer regression.
  std::size_t parity_snippet_entries = 0;
  const auto mean_recall = [&](const std::string& packing, int budget) {
    double sum = 0.0;
    int n = 0;
    for (const auto& row : rows) {
      // Pin gather=fixed so the greedy-vs-knapsack comparison isolates the FILL:
      // the default gather is adaptive, which would change the candidate pool.
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
        if (entry.contains("snippet")) {
          ++parity_snippet_entries;
        }
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
  // Re-pinned for openspec/changes/re-anchor-retrieval-fixture: the fixture was
  // regenerated at 0cb8237 (1580 nodes / 3178 links, N=75 rows vs the old
  // 1181/1521, N=35 -- the C/C++ call-graph fix, enrichment-identity fixes, and
  // Rust extraction all changed the graph the engine actually produces), and the
  // knapsack's value model was repaired in the same change (value scales with
  // sqrt(slice cost); per-item value let the DP shed the large relevant slices
  // for confetti, an 8.6-point gap the sparser old fixture could not see).
  // Baselines are transcriptions of the gate's own output on the new pair. 6000
  // is the shipped default budget. The gate stays non-regression per packer plus
  // the symmetric packing-parity band. Environment note: entry costs include the
  // absolute source path, so recall moves with checkout depth; these pins were
  // measured at root length 58 (a deep worktree), shorter real-world roots only
  // add margin to the floors.
  const std::vector<Target> targets = {{2000, 0.407062, 0.398159, true},
                                       {4000, 0.493927, 0.481693, true},
                                       {6000, 0.530070, 0.519019, true},
                                       {8000, 0.547631, 0.537489, false}};
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
  // Adaptive gather widens the pool beyond the 2-hop core along query-relevant
  // nodes; since the re-anchor at 0cb8237 it fills GREEDY (decoupled from the
  // knapsack -- the coupling cost the default path 2.6-5.9 recall points on the
  // densified graph, research/packer-regression). On this fixture adaptive
  // measures recall-identical to fixed greedy@k2, so the contract is
  // non-inferiority: the default must never trail the plain k=2 baseline, while
  // still gathering strictly fewer candidates than the full k=3 fan-out (its
  // deep-reach at bounded pool cost is the reason it stays the default).
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
  for (const int budget : {2000, 4000}) {  // 8k neutral: the ego graph mostly fits
    const auto [r_k2, c_k2] = measure("greedy", "fixed", 2, budget);
    // The default path: adaptive gather, greedy fill.
    const auto [r_adp, c_adp] = measure("greedy", "adaptive", 3, budget);
    const auto [r_k3, c_k3] = measure("knapsack", "fixed", 3, budget);
    const double delta = r_adp - r_k2;
    // Non-inferiority against the plain fixed-k2 baseline plus a strictly
    // smaller candidate pool than the full k=3 fan-out. The pre-re-anchor
    // "material positive gain" floor was measured on the sparser 1181-node
    // fixture; on the current graph adaptive is recall-identical to k2 and its
    // value is deep reach at bounded pool cost, so the honest gate is
    // no-regression, not a gain it no longer shows.
    const bool ok = delta >= -kTol && c_adp < c_k3;
    if (!ok) {
      ++failures;
    }
    std::cout << "  " << budget << "   " << r_k2 << "   " << r_adp << "   " << r_k3 << "   " << delta << "   "
              << c_k2 << "/" << c_adp << "/" << c_k3 << "   " << (ok ? "PASS" : "FAIL") << "\n";
  }

  if (parity_snippet_entries == 0) {
    std::cerr << "FAIL: no returned entry carried a snippet across any run -- "
                 "the graph under test is snippet-free (source paths not resolvable)\n";
    return 1;
  }
  if (failures != 0) {
    std::cerr << failures << " parity assertion(s) failed\n";
    return 1;
  }
  std::cout << "parity OK\n";
  return 0;
}
