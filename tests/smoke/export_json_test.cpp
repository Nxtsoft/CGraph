#include "cgraph/export_json.hpp"

int main() {
  cgraph::GraphSnapshot graph;
  graph.nodes.push_back(cgraph::Node{.id = "a", .label = "Alpha", .source_file = "a.cpp", .kind = "class"});
  graph.nodes.push_back(cgraph::Node{.id = "b", .label = "Beta", .source_file = "b.cpp", .kind = "function"});
  graph.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "CALLS"});

  const auto json = cgraph::to_node_link_json(graph);
  if (json["directed"] != true || json["multigraph"] != false) {
    return 1;
  }
  if (!json["nodes"].is_array() || json["nodes"].size() != 2) {
    return 1;
  }
  if (!json["links"].is_array() || json["links"].size() != 1) {
    return 1;
  }
  if (json["links"][0]["source"] != "a" || json["links"][0]["target"] != "b") {
    return 1;
  }

  const auto html = cgraph::export_graph_html(graph);
  if (html.find("<html") == std::string::npos ||
      html.find("Alpha") == std::string::npos ||
      html.find("<canvas id=\"graph-canvas\"") == std::string::npos ||
      html.find("getContext(\"2d\")") == std::string::npos ||
      html.find("const graphData = ") == std::string::npos ||
      html.find("Search nodes") == std::string::npos ||
      html.find("node-details") == std::string::npos ||
      html.find("draggingNodeId") == std::string::npos ||
      html.find("communityFor(") == std::string::npos ||
      html.find("highlightIdsFor(") == std::string::npos ||
      html.find("<svg id=\"graph\"") != std::string::npos ||
      html.find("<ul>") != std::string::npos) {
    return 1;
  }

  // Selection is reversible: Escape + empty-canvas clear selection, and the
  // view exposes reset/fit controls (improve-graph-html-view, task group 1).
  if (html.find("function clearSelection(") == std::string::npos ||
      html.find("\"keydown\"") == std::string::npos ||
      html.find("\"Escape\"") == std::string::npos ||
      html.find("function resetView(") == std::string::npos ||
      html.find("function fitToScreen(") == std::string::npos ||
      html.find("Reset view") == std::string::npos ||
      html.find("Fit to screen") == std::string::npos) {
    return 1;
  }

  // Labels are bounded by a top-by-degree budget rather than a bare radius
  // gate, so an overview is not a wall of text (task group 2).
  if (html.find("labelBudget") == std::string::npos) {
    return 1;
  }

  // Light/dark theme toggle: a control flips a theme attribute and the canvas
  // colors follow CSS variables rather than hardcoded values (group 5). Light is
  // opt-in via data-theme="light"; dark is the unconditional default.
  if (html.find("theme-toggle") == std::string::npos ||
      html.find("data-theme=\"light\"") == std::string::npos ||
      html.find("--canvas-bg") == std::string::npos) {
    return 1;
  }

  // Graphify visual parity: dark-by-default canvas (#0f0f1a), the Tableau-10
  // community palette, the full right sidebar (Node Info + Communities headings
  // + stats footer), and no leftover top header bar. These pin the viewer's look
  // to the reference so a future restyle that drifts away is caught.
  if (html.find("--canvas-bg: #0f0f1a") == std::string::npos ||      // dark by default
      html.find("color-scheme: dark") == std::string::npos ||
      html.find("#4E79A7") == std::string::npos ||                   // Tableau-10 lead color
      html.find("#F28E2B") == std::string::npos ||                   // Tableau-10 orange
      html.find(">Node Info<") == std::string::npos ||
      html.find(">Communities<") == std::string::npos ||
      html.find("id=\"stats\"") == std::string::npos ||
      html.find("<header>") != std::string::npos) {                  // header removed
    return 1;
  }

  // Large graphs open community-collapsed (report-modules): a super-node per
  // community above the threshold, expanded by click/search/"Expand all".
  if (html.find("COLLAPSE_THRESHOLD = 500") == std::string::npos ||
      html.find("function rebuildSuperNodes(") == std::string::npos ||
      html.find("function expandCommunity(") == std::string::npos ||
      html.find("id=\"collapse-toggle\"") == std::string::npos) {
    return 1;
  }

  // Collapsed edges aggregate per drawn thing, not per community. Two expanded
  // members calling into the same disc must keep one arrow each, so an expanded
  // endpoint contributes its own node id; the superseded `(a.community || a.id)`
  // key was always the community pair and drew only one of them.
  if (html.find("const key = drawnKey(source) + \"\\u0000\" + drawnKey(target);") == std::string::npos ||
      html.find("function drawnKey(node) { return isCollapsed(node) ? COMMUNITY_ID_PREFIX + node.community : node.id; }") ==
          std::string::npos ||
      html.find("(a.community || a.id)") != std::string::npos) {
    return 1;
  }

  // Hovering a super-node resolves to its community's members and everything
  // they touch; without the prefix branch the lookup misses and every node dims.
  if (html.find("if (id.startsWith(COMMUNITY_ID_PREFIX)) {") == std::string::npos) {
    return 1;
  }

  // The viewer maps an arbitrary coordinate scale (unit or pixel): a precomputed
  // layout's span is normalized onto the canvas, fitToScreen is unclamped, and
  // the zoom bounds are relative to the fit. Without the normalization a
  // unit-scale graph.json paints as one blob or as overlapping giant discs.
  if (html.find("function normalizeLayoutSpan(") == std::string::npos ||
      html.find("    normalizeLayoutSpan();") == std::string::npos ||
      html.find("fitScale = scale;") == std::string::npos ||
      html.find("Math.min(4,") != std::string::npos ||
      html.find("Math.min(fitScale * 4, transform.scale * factor)") == std::string::npos) {
    return 1;
  }

  // Super-node labels are screen-constant like every other label: divided by
  // the current zoom, not fixed px inside the scaled transform.
  if (html.find("\"bold \" + (12 / transform.scale) + \"px") == std::string::npos ||
      html.find("(11 / transform.scale) + \"px") == std::string::npos) {
    return 1;
  }

  // A disc pushed off an overlapping neighbour carries its members, so
  // expanding it reveals them where the disc sat.
  if (html.find("const dx = disc.x - centroid[i].x;") == std::string::npos ||
      html.find("member.x += dx;") == std::string::npos) {
    return 1;
  }

  // Legend is a dynamic per-community color key, not two hardcoded rows.
  if (html.find("buildLegend(") == std::string::npos ||
      html.find("id=\"legend\"") == std::string::npos) {
    return 1;
  }

  // Layout is community-aware and deterministic: a community-centroid force
  // pulls clusters together, and no Math.random is used (task group 3).
  if (html.find("communityCentroid") == std::string::npos ||
      html.find("Math.random(") != std::string::npos) {
    return 1;
  }

  // Regression guard: nodes must relax in open world space, never clamp to the
  // canvas walls (which previously pinned outward-pushed nodes into a dense
  // rectangular border). The invariant is documented at the node-move step; if a
  // future edit reintroduces a viewport clamp it has to delete this contract.
  if (html.find("No hard wall clamp") == std::string::npos) {
    return 1;
  }

  const auto svg = cgraph::export_graph_svg(graph);
  if (svg.find("<svg") == std::string::npos || svg.find("<circle") == std::string::npos) {
    return 1;
  }

  const auto obsidian = cgraph::export_obsidian_markdown(graph);
  if (obsidian.find("# Alpha") == std::string::npos || obsidian.find("[[b]]") == std::string::npos) {
    return 1;
  }

  const auto cypher = cgraph::export_neo4j_cypher(graph);
  if (cypher.find("MERGE (n:Symbol") == std::string::npos || cypher.find("CALLS") == std::string::npos) {
    return 1;
  }

  const auto call_flow = cgraph::export_call_flow_html(graph);
  if (call_flow.find("a calls b") == std::string::npos) {
    return 1;
  }

  return 0;
}
