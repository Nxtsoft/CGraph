// End-to-end retrieval quality gate (openspec: end-to-end-retrieval-gate).
//
// The parity gate (pack_context_parity_test) injects each row's grade-2 focal id,
// isolating gather/packing. This gate deliberately does not: it sends only the
// row's free-text query through the `context` op (`{q, budget}`, engine defaults
// for gather/packing/depth), so focal resolution -- matching_nodes + the lexical
// multi-seed fallback -- is on the measured path. The route-2 entity-routing bug
// (3efa8e0) lived exactly there and was invisible to the parity gate.
//
// Metric: mean grade-2 recall@budget over the committed fixture rows
// (tests/fixtures/pack_context_parity/{graph.json,queries.jsonl}); rows whose
// query resolves no focal count as recall 0 -- resolution failure is the
// regression class this gate exists to catch, so it must not be skipped.
//
// Baselines are the values measured on the committed fixture at gate
// introduction; the assertion is non-regression (measured >= baseline - kTol),
// not an aspirational threshold.

#include "cgraph/daemon_lifecycle.hpp"
#include "cgraph/daemon_ops.hpp"
#include "cgraph/protocol.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
  const fs::path fixture = CGRAPH_PARITY_FIXTURE_DIR;
  const fs::path graph_path = fixture / "graph.json";
  const fs::path eval_path = fixture / "queries.jsonl";

  if (!fs::exists(graph_path) || !fs::exists(eval_path)) {
    std::cout << "SKIP: fixture absent (" << graph_path << " / " << eval_path << ").\n";
    return 0;  // Defensive fallback; the fixture is committed.
  }

  cgraph::DaemonState state;
  if (!cgraph::load_graph_snapshot(state, graph_path)) {
    std::cerr << "FAIL: could not load " << graph_path << "\n";
    return 1;
  }
  // Rebase the fixture's repository-relative source paths onto the real repo root,
  // exactly as the parity gate does. Without this the gate's snippets resolve only
  // when the CWD happens to be the repo root -- ctest runs from the build tree, so
  // the CI run measured a snippet-free graph and the budget never bound.
  auto portable_graph = *cgraph::read_graph_snapshot(state);
  const fs::path repo_root = CGRAPH_REPO_ROOT;
  for (auto& node : portable_graph.nodes) {
    if (node.source_file.empty()) {
      continue;
    }
    const fs::path source_file = node.source_file;
    if (source_file.is_absolute()) {
      std::cerr << "FAIL: fixture source paths must be repository-relative: " << source_file << "\n";
      return 1;
    }
    node.source_file = (repo_root / source_file).lexically_normal().string();
  }
  cgraph::publish_graph_snapshot(state, std::move(portable_graph));

  struct Row {
    std::string query;
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
    if (!grade2.empty()) {
      rows.push_back({row.value("query", std::string{}), std::move(grade2)});
    }
  }

  // Mean grade-2 recall through the default end-to-end path: query text only.
  // snippet_entries guards against the CWD regression this gate once had: if no
  // returned entry ever resolves a snippet, the graph under test is snippet-free
  // and every recall number below is measured on the wrong surface.
  std::size_t snippet_entries = 0;
  const auto mean_recall = [&](int budget) {
    double sum = 0.0;
    for (const auto& row : rows) {
      const nlohmann::json params{{"q", row.query}, {"budget", budget}};
      const auto response = cgraph::handle_daemon_request(state, cgraph::make_request("context", params));
      std::set<std::string> selected;
      if (response.contains("result")) {
        const auto& result = response["result"];
        if (result.contains("focus") && result["focus"].is_object()) {
          selected.insert(result["focus"].value("id", std::string{}));
        }
        for (const auto& entry : result.value("included", nlohmann::json::array())) {
          selected.insert(entry.value("id", std::string{}));
          if (entry.contains("snippet")) {
            ++snippet_entries;
          }
        }
      }
      std::size_t hit = 0;
      for (const auto& id : row.grade2) {
        if (selected.count(id) != 0) {
          ++hit;
        }
      }
      // A query that resolves nothing contributes 0 -- counted, never skipped.
      sum += static_cast<double>(hit) / static_cast<double>(row.grade2.size());
    }
    return rows.empty() ? 0.0 : sum / static_cast<double>(rows.size());
  };

  struct Target {
    int budget;
    double baseline;  // measured on the committed fixture at gate introduction
  };
  // Re-pinned for openspec/changes/re-anchor-retrieval-fixture: the fixture was
  // regenerated at 0cb8237 (1580 nodes / 3178 links, 75 symbol rows mined from
  // the full history vs 35 through June 15), and the same change decoupled the
  // default adaptive gather from the knapsack fill -- the coupling was shedding
  // the large relevant slices for confetti on the densified graph, and its
  // removal roughly doubled measured end-to-end recall at every budget (e.g.
  // 0.0995 -> 0.1804 at 2000 on this same pair). Baselines are transcriptions
  // of the gate's own output on the new pair, measured at root length 58; 6000
  // is the shipped default budget.
  const std::vector<Target> targets = {
      {2000, 0.1804}, {4000, 0.2330}, {6000, 0.2795}, {8000, 0.2936}};
  constexpr double kTol = 0.03;

  std::cout << "end-to-end retrieval gate  (N=" << rows.size() << " symbol rows, q-only, engine defaults)\n";
  std::cout << "budget   recall   baseline   gate\n";
  int failures = 0;
  for (const auto& t : targets) {
    const double recall = mean_recall(t.budget);
    const bool ok = recall + 1e-9 >= t.baseline - kTol;
    if (!ok) {
      ++failures;
    }
    std::cout << "  " << t.budget << "   " << recall << "   " << t.baseline << "   "
              << (ok ? "PASS" : "FAIL") << "\n";
  }

  if (snippet_entries == 0) {
    std::cerr << "FAIL: no returned entry carried a snippet across any budget -- "
                 "the graph under test is snippet-free (source paths not resolvable)\n";
    return 1;
  }

  if (failures != 0) {
    std::cerr << failures << " end-to-end recall assertion(s) below baseline\n";
    return 1;
  }
  std::cout << "end-to-end retrieval OK\n";
  return 0;
}
