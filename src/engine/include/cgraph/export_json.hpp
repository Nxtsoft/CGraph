#pragma once

#include "cgraph/types.hpp"

#include <nlohmann/json.hpp>

namespace cgraph {

[[nodiscard]] nlohmann::json to_node_link_json(const GraphSnapshot& graph);
// Inverse of to_node_link_json: parse a node-link graph (the graph.json shape, with
// "links") back into a GraphSnapshot. Shared by the daemon fast-load path and seam fuse.
[[nodiscard]] GraphSnapshot parse_node_link_graph(const nlohmann::json& value);
[[nodiscard]] std::string export_graph_html(const GraphSnapshot& graph);
[[nodiscard]] std::string export_graph_svg(const GraphSnapshot& graph);
[[nodiscard]] std::string export_obsidian_markdown(const GraphSnapshot& graph);
[[nodiscard]] std::string export_neo4j_cypher(const GraphSnapshot& graph);

}  // namespace cgraph
