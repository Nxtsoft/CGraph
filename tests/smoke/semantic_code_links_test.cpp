#include "cgraph/semantic_code_links.hpp"

#include <algorithm>
#include <string>

namespace {

bool has_id(const std::vector<cgraph::CandidateLink>& links, const std::string& id) {
  return std::ranges::any_of(links, [&](const cgraph::CandidateLink& l) { return l.node_id == id; });
}

cgraph::Node node(std::string id, std::string label, std::string kind) {
  cgraph::Node n;
  n.id = std::move(id);
  n.label = std::move(label);
  n.kind = std::move(kind);
  return n;
}

}  // namespace

int main() {
  using namespace cgraph;

  GraphSnapshot graph;
  graph.nodes.push_back(node("n:ccf", "classify_cached_file(const std::filesystem::path&)", "function"));
  graph.nodes.push_back(node("n:gs", "GraphSnapshot", "struct"));
  graph.nodes.push_back(node("n:frag", "Fragment", "struct"));
  graph.nodes.push_back(node("n:cache", "cache", "variable"));  // bare lowercase word
  // A bare-word name shared by many nodes (ambiguous + low specificity).
  for (int i = 0; i < 10; ++i) {
    graph.nodes.push_back(node("n:value" + std::to_string(i), "value", "variable"));
  }
  // A COMPOUND name shared by more nodes than the old kMaxNodesPerName cliff (8).
  // Sharing a name is low specificity, not evidence against a mention.
  for (int i = 0; i < 9; ++i) {
    graph.nodes.push_back(node("n:sf" + std::to_string(i), "source_file", "field"));
  }
  // A compound name shared by exactly two nodes (specific-ish).
  graph.nodes.push_back(node("n:run_one_shot", "run_one_shot()", "function"));

  const auto index = build_symbol_index(graph);

  // 1. A compound (snake_case) symbol mentioned in prose -> candidate.
  {
    const auto links = compute_candidate_links("The planner calls classify_cached_file on each file.", index);
    if (!has_id(links, "n:ccf")) {
      return 1;
    }
  }

  // 2. A capitalized type name (CamelCase) -> candidate.
  {
    const auto links = compute_candidate_links("Everything links against the GraphSnapshot type.", index);
    if (!has_id(links, "n:gs")) {
      return 2;
    }
  }

  // 3. A capitalized single-word TYPE -> kept via the type-like rule.
  {
    const auto links = compute_candidate_links("A Fragment carries nodes and edges.", index);
    if (!has_id(links, "n:frag")) {
      return 3;
    }
  }

  // 4. A bare lowercase word that merely collides with a symbol name -> dropped.
  {
    const auto links = compute_candidate_links("We cache the result for speed.", index);
    if (has_id(links, "n:cache")) {
      return 4;  // 'cache' must not produce a candidate by itself
    }
  }

  // 5. A bare lowercase name stays out however many nodes share it -- the shape
  //    filter, not a node-count cliff, is what excludes it.
  {
    const auto links = compute_candidate_links("The value of the value is the value.", index);
    if (std::ranges::any_of(links, [](const CandidateLink& l) { return l.node_id.rfind("n:value", 0) == 0; })) {
      return 5;
    }
  }

  // 6. Cap is honored.
  {
    const auto links = compute_candidate_links(
        "classify_cached_file GraphSnapshot Fragment run_one_shot", index, /*max_links=*/2);
    if (links.size() != 2) {
      return 6;
    }
  }

  // 7. Empty / no-mention text -> no candidates.
  {
    if (!compute_candidate_links("", index).empty() ||
        !compute_candidate_links("plain prose with no symbols here", index).empty()) {
      return 7;
    }
  }

  // 8. A compound name shared by 9 nodes -- more than the old kMaxNodesPerName
  //    cliff of 8 -- still produces candidates. Measured against this repo's own
  //    markdown (397 docs, 15,311 doc/token pairs labelled by whether the author
  //    wrote the token in backticks), dropping the cliff moved precision 0.873 ->
  //    0.878 and recall 0.354 -> 0.371: it cost recall and bought no precision,
  //    because the names it blocked were mostly genuine references. `source_file`
  //    is the clearest case -- compound, unambiguous in prose, and discarded for
  //    all 61 of its mentions purely because 9 nodes carry the name.
  {
    const auto links = compute_candidate_links(
        "Each node records the source_file it came from.", index, /*max_links=*/16);
    if (!has_id(links, "n:sf0")) {
      return 8;
    }
  }

  return 0;
}
