#include "cgraph/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <string>

int main() {
  cgraph::GraphSnapshot graph;
  graph.nodes.push_back(cgraph::Node{.id = "a", .label = "A"});
  graph.nodes.push_back(cgraph::Node{.id = "b", .label = "B"});
  graph.nodes.push_back(cgraph::Node{.id = "c", .label = "C"});
  graph.nodes.push_back(cgraph::Node{.id = "d", .label = "D"});
  graph.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "LINKS"});
  graph.edges.push_back(cgraph::Edge{.source = "c", .target = "d", .relation = "LINKS"});

  const auto result = cgraph::detect_communities(graph);
  if (result.cluster_count < 1) {
    return 1;
  }
  for (const auto& node : graph.nodes) {
    if (!node.properties.contains("community")) {
      return 1;
    }
  }
  // The embedded layout is canvas-scale: the wider axis spans the 720px
  // minimum side (igraph's raw unit-scale output would span a few units and
  // render as one blob), centered on the origin.
  {
    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (const auto& node : graph.nodes) {
      if (!node.properties.contains("x") || !node.properties.contains("y")) {
        return 6;
      }
      const double x = std::stod(node.properties.at("x"));
      const double y = std::stod(node.properties.at("y"));
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
    const double span = std::max(max_x - min_x, max_y - min_y);
    if (span < 719.0 || span > 721.0 || std::abs(min_x + max_x) > 0.02 || std::abs(min_y + max_y) > 0.02) {
      return 7;
    }
  }

  graph.edges.push_back(cgraph::Edge{.source = "a", .target = "c", .relation = "CROSSES"});
  cgraph::analyze_graph(graph);
  bool saw_god_node = false;
  for (const auto& node : graph.nodes) {
    if (!node.properties.contains("degree_centrality")) {
      return 1;
    }
    if (node.properties.contains("god_node")) {
      saw_god_node = true;
    }
  }
  if (!saw_god_node) {
    return 1;
  }
  bool saw_cross_community = false;
  for (const auto& edge : graph.edges) {
    if (edge.properties.contains("cross_community")) {
      saw_cross_community = true;
    }
  }
  if (!saw_cross_community) {
    return 1;
  }

  cgraph::GraphSnapshot empty;
  const auto empty_result = cgraph::detect_communities(empty);
  if (empty_result.cluster_count != 0) {
    return 1;
  }

  // Session-memory nodes are inert to analysis: they receive no centrality or
  // god_node, and adding one (with a concerns edge to code) does not shift any
  // code node's centrality.
  {
    const auto build = []() {
      cgraph::GraphSnapshot g;
      g.nodes.push_back(cgraph::Node{.id = "fn:a", .label = "a"});
      g.nodes.push_back(cgraph::Node{.id = "fn:b", .label = "b"});
      g.edges.push_back(cgraph::Edge{.source = "fn:a", .target = "fn:b", .relation = "CALLS"});
      return g;
    };
    auto baseline = build();
    cgraph::analyze_graph(baseline);

    auto with_memory = build();
    with_memory.nodes.push_back(cgraph::Node{.id = "memory:checkpoint:1", .label = "cp", .kind = "checkpoint"});
    with_memory.edges.push_back(
        cgraph::Edge{.source = "memory:checkpoint:1", .target = "fn:a", .relation = "concerns"});
    cgraph::analyze_graph(with_memory);

    const auto centrality_of = [](const cgraph::GraphSnapshot& g, const std::string& id) {
      for (const auto& node : g.nodes) {
        if (node.id == id) {
          const auto it = node.properties.find("degree_centrality");
          return it == node.properties.end() ? std::string{"<none>"} : it->second;
        }
      }
      return std::string{"<missing>"};
    };
    // Code-node centrality is identical with and without the memory node + edge.
    if (centrality_of(baseline, "fn:a") != centrality_of(with_memory, "fn:a") ||
        centrality_of(baseline, "fn:b") != centrality_of(with_memory, "fn:b")) {
      return 2;
    }
    // The memory node itself carries neither centrality nor god_node.
    for (const auto& node : with_memory.nodes) {
      if (node.id == "memory:checkpoint:1" &&
          (node.properties.contains("degree_centrality") || node.properties.contains("god_node"))) {
        return 3;
      }
    }
  }

  return 0;
}
