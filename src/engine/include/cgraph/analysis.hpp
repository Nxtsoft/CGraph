#pragma once

#include "cgraph/types.hpp"

namespace cgraph {

struct CommunityResult {
  bool used_leiden = false;
  int cluster_count = 0;
  double quality = 0.0;
};

// The span write_layout gives a precomputed layout: enough canvas for the node
// count that ink per node stays constant as the graph grows. graph.html restates
// any other producer's coordinates in these terms (export_json.cpp,
// normalizeLayoutSpan), so the policy has one definition.
inline constexpr double kMinCanvasSide = 720.0;
inline constexpr double kPixelsPerSqrtNode = 30.0;
// Fruchterman-Reingold iterations for the precomputed layout, at every graph
// size (fix-layout-cliff): there is no second algorithm and no size threshold.
inline constexpr int kLayoutIterations = 500;

[[nodiscard]] CommunityResult detect_communities(GraphSnapshot& graph);
void analyze_graph(GraphSnapshot& graph);

}  // namespace cgraph
