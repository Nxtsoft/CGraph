#include "cgraph/export_json.hpp"

#include "cgraph/fragment_json.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cgraph {
namespace {

[[nodiscard]] BuildState parse_build_state(int value) {
  switch (value) {
    case static_cast<int>(BuildState::DeterministicReady):
      return BuildState::DeterministicReady;
    case static_cast<int>(BuildState::Enriching):
      return BuildState::Enriching;
    case static_cast<int>(BuildState::Idle):
      return BuildState::Idle;
    case static_cast<int>(BuildState::Failed):
      return BuildState::Failed;
    case static_cast<int>(BuildState::Empty):
    default:
      return BuildState::Empty;
  }
}

[[nodiscard]] Node parse_node_link_node(const nlohmann::json& value) {
  Node node;
  node.id = value.value("id", std::string{});
  node.label = value.value("label", node.id);
  node.source_file = value.value("source_file", std::string{});
  node.kind = value.value("type", value.value("kind", std::string{}));
  // Round-trip the source span: without it a fast-loaded snapshot serves
  // location-less, snippet-less context, and starves slice-cost knapsack packing.
  if (const auto iter = value.find("source_location"); iter != value.end() && iter->is_object()) {
    SourceLocation location;
    location.start_line = iter->value("start_line", 0U);
    location.start_column = iter->value("start_column", 0U);
    location.end_line = iter->value("end_line", 0U);
    location.end_column = iter->value("end_column", 0U);
    node.source_location = location;
  }
  if (const auto iter = value.find("confidence"); iter != value.end() && iter->is_string()) {
    Confidence confidence = Confidence::Extracted;
    if (parse_confidence(iter->get<std::string>(), confidence)) {
      node.confidence = confidence;
    }
  }
  if (const auto iter = value.find("confidence_score"); iter != value.end() && iter->is_number()) {
    node.confidence_score = iter->get<double>();
  }
  if (const auto iter = value.find("properties"); iter != value.end() && iter->is_object()) {
    for (const auto& [key, property] : iter->items()) {
      node.properties.emplace(key, property.is_string() ? property.get<std::string>() : property.dump());
    }
  }
  return node;
}

[[nodiscard]] Edge parse_node_link_edge(const nlohmann::json& value) {
  Edge edge;
  edge.source = value.value("source", std::string{});
  edge.target = value.value("target", std::string{});
  edge.relation = value.value("relation", std::string{});
  if (const auto iter = value.find("confidence"); iter != value.end() && iter->is_string()) {
    Confidence confidence = Confidence::Extracted;
    if (parse_confidence(iter->get<std::string>(), confidence)) {
      edge.confidence = confidence;
    }
  }
  if (const auto iter = value.find("confidence_score"); iter != value.end() && iter->is_number()) {
    edge.confidence_score = iter->get<double>();
  }
  if (const auto iter = value.find("properties"); iter != value.end() && iter->is_object()) {
    for (const auto& [key, property] : iter->items()) {
      edge.properties.emplace(key, property.is_string() ? property.get<std::string>() : property.dump());
    }
  }
  return edge;
}

[[nodiscard]] std::string html_escape(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const auto ch : value) {
    switch (ch) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  return output;
}

[[nodiscard]] std::string cypher_escape(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const auto ch : value) {
    if (ch == '\\' || ch == '\'') {
      output.push_back('\\');
    }
    output.push_back(ch);
  }
  return output;
}

[[nodiscard]] std::unordered_map<std::string, std::size_t> node_index(const GraphSnapshot& graph) {
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    index.emplace(graph.nodes[i].id, i);
  }
  return index;
}

// Dump the JSON once and escape any "</" sequence (so the embedded payload
// cannot close the surrounding <script> tag) while appending directly into the
// caller's buffer. Appending in place avoids materializing a second full copy
// of the multi-megabyte serialized graph.
void append_json_for_script(std::string& output, const nlohmann::json& value) {
  const auto json = value.dump();
  output.reserve(output.size() + json.size());
  for (std::size_t index = 0; index < json.size(); ++index) {
    if (json[index] == '<' && index + 1 < json.size() && json[index + 1] == '/') {
      output += "<\\/";
      ++index;
      continue;
    }
    output.push_back(json[index]);
  }
}

}  // namespace

GraphSnapshot parse_node_link_graph(const nlohmann::json& value) {
  GraphSnapshot graph;
  const auto meta = value.value("graph", nlohmann::json::object());
  graph.build_state = parse_build_state(meta.value("build_state", static_cast<int>(BuildState::Empty)));
  graph.cache_hit_rate = meta.value("cache_hit_rate", 0.0);

  for (const auto& node : value.value("nodes", nlohmann::json::array())) {
    graph.nodes.push_back(parse_node_link_node(node));
  }
  for (const auto& edge : value.value("links", nlohmann::json::array())) {
    graph.edges.push_back(parse_node_link_edge(edge));
  }
  return graph;
}

nlohmann::json to_node_link_json(const GraphSnapshot& graph) {
  auto nodes = nlohmann::json::array();
  for (const auto& node : graph.nodes) {
    auto value = to_json(node);
    value["id"] = node.id;
    nodes.push_back(std::move(value));
  }

  auto links = nlohmann::json::array();
  for (const auto& edge : graph.edges) {
    auto value = to_json(edge);
    value["source"] = edge.source;
    value["target"] = edge.target;
    links.push_back(std::move(value));
  }

  return nlohmann::json{
      {"directed", true},
      {"multigraph", false},
      {"graph", {{"build_state", static_cast<int>(graph.build_state)}, {"cache_hit_rate", graph.cache_hit_rate}}},
      {"nodes", std::move(nodes)},
      {"links", std::move(links)},
  };
}

std::string export_graph_html(const GraphSnapshot& graph) {
  std::string output;
  // The embedded graph JSON dominates the document; pre-size generously (about
  // 1KB/node + 0.2KB/edge plus the static shell) so the buffer grows at most
  // once instead of reallocating its way up to tens of megabytes.
  output.reserve(64 * 1024 + graph.nodes.size() * 1024 + graph.edges.size() * 256);
  output += R"html(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>cgraph</title>
<style>
  :root {
    /* Dark by default to match the Graphify viewer's signature look (it is
       dark-only); light remains available via the toggle below. */
    color-scheme: dark;
    --bg: #0f0f1a;
    --panel: #1a1a2e;
    --ink: #e0e0e0;
    --muted: #888888;
    --line: #2a2a4e;
    --edge: #3a3a5e;
    --accent: #4E79A7;
    --class: #4E79A7;
    --function: #59A14F;
    --other: #B07AA1;
    --focus: #EDC948;
    /* Graph canvas colors are variables so light/dark theming flows through to
       the canvas, which cannot inherit CSS the way DOM nodes do. */
    --canvas-bg: #0f0f1a;
    --graph-text: #ffffff;
    --graph-edge: #5a6282;
    --node-stroke: #0f0f1a;
  }
  /* Light is opt-in via the toggle only. Dark stays the default unconditionally
     (not OS-dependent) so the viewer always opens in the Graphify look — the
     reference viewer is dark-only. */
  :root[data-theme="light"] {
    color-scheme: light;
    --bg: #f6f7f9;
    --panel: #ffffff;
    --ink: #17202a;
    --muted: #667085;
    --line: #d7dde5;
    --edge: #9aa6b2;
    --accent: #4E79A7;
    --canvas-bg: #fbfcfd;
    --graph-text: #111827;
    --graph-edge: #9aa6b2;
    --node-stroke: #ffffff;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    color: var(--ink);
    background: var(--bg);
  }
  /* Full-bleed canvas + fixed right sidebar, matching the Graphify viewer's
     chrome-less layout (no top header bar). */
  .shell {
    height: 100vh;
    display: flex;
    overflow: hidden;
  }
  .stage {
    position: relative;
    flex: 1;
    min-width: 0;
    overflow: hidden;
  }
  #graph-canvas {
    width: 100%;
    height: 100%;
    min-height: 620px;
    display: block;
    background: var(--canvas-bg);
    cursor: grab;
  }
  #graph-canvas:active {
    cursor: grabbing;
  }
  /* Subtle, low-key on-canvas chrome so the graph reads first (Graphify keeps
     the canvas clean; these stay out of the way). */
  .hint {
    position: absolute;
    left: 14px;
    bottom: 12px;
    color: var(--muted);
    opacity: 0.6;
    font-size: 12px;
    max-width: 60%;
    pointer-events: none;
  }
  .controls {
    position: absolute;
    right: 14px;
    top: 12px;
    display: flex;
    gap: 6px;
  }
  .control-btn {
    border: 1px solid var(--line);
    background: rgba(26, 26, 46, 0.7);
    color: var(--muted);
    border-radius: 6px;
    padding: 5px 9px;
    font: inherit;
    font-size: 12px;
    cursor: pointer;
  }
  .control-btn:hover {
    color: var(--ink);
    border-color: var(--accent);
  }
  /* Right sidebar: Search -> Node Info -> Communities -> stats footer, 280px,
     matching the Graphify layout. */
  aside {
    width: 280px;
    flex-shrink: 0;
    border-left: 1px solid var(--line);
    background: var(--panel);
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }
  .search-wrap {
    padding: 12px;
    border-bottom: 1px solid var(--line);
  }
  input {
    width: 100%;
    min-width: 0;
    background: var(--bg);
    border: 1px solid var(--edge);
    border-radius: 6px;
    padding: 7px 10px;
    font: inherit;
    font-size: 13px;
    color: var(--ink);
    outline: none;
  }
  input:focus {
    border-color: var(--accent);
  }
  .info-panel {
    padding: 14px;
    border-bottom: 1px solid var(--line);
    min-height: 120px;
  }
  .panel-head {
    font-size: 13px;
    color: var(--muted);
    margin: 0 0 8px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    font-weight: 600;
  }
  #node-details {
    min-width: 0;
    overflow: auto;
    font-size: 13px;
    line-height: 1.6;
  }
  .legend-wrap {
    flex: 1;
    overflow-y: auto;
    padding: 12px;
  }
  .legend {
    display: flex;
    flex-direction: column;
    gap: 2px;
    color: var(--ink);
    font-size: 12px;
  }
  .legend .row {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 4px 0;
    border-radius: 4px;
  }
  .legend .legend-item:hover {
    background: var(--line);
    padding-left: 4px;
  }
  .legend .lbl {
    flex: 1;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .legend .count {
    margin-left: auto;
    padding-left: 10px;
    color: var(--muted);
    font-size: 11px;
    font-variant-numeric: tabular-nums;
  }
  .legend .foot {
    margin-top: 8px;
    padding-top: 8px;
    border-top: 1px solid var(--line);
    color: var(--muted);
  }
  .legend .dot {
    width: 12px;
    height: 12px;
    border-radius: 50%;
    flex: none;
  }
  .stats {
    padding: 10px 14px;
    border-top: 1px solid var(--line);
    font-size: 11px;
    color: var(--muted);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .detail-title {
    margin: 0 0 6px;
    font-size: 15px;
    font-weight: 650;
    overflow-wrap: anywhere;
  }
  .detail-kind {
    display: inline-block;
    margin-bottom: 12px;
    color: var(--muted);
  }
  .field {
    margin: 8px 0;
  }
  .field strong {
    display: block;
    margin-bottom: 3px;
    color: var(--muted);
    font-size: 12px;
  }
  .field code {
    display: block;
    overflow-wrap: anywhere;
    white-space: normal;
    color: var(--ink);
  }
  .empty {
    color: var(--muted);
    font-style: italic;
  }
  @media (max-width: 840px) {
    .shell {
      flex-direction: column;
    }
    aside {
      width: auto;
      border-left: 0;
      border-top: 1px solid var(--line);
    }
  }
</style>
</head>
<body>
<div class="shell">
  <section class="stage" aria-label="Graph visualization">
    <canvas id="graph-canvas" aria-label="Interactive graph visualization"></canvas>
    <div class="controls">
      <button type="button" class="control-btn" id="fit-view">Fit to screen</button>
      <button type="button" class="control-btn" id="reset-view">Reset view</button>
      <button type="button" class="control-btn" id="theme-toggle" aria-pressed="false">Light</button>
    </div>
    <div class="hint">Drag a node to rearrange. Scroll to zoom, drag to pan. Click a node for details. Esc clears.</div>
  </section>
  <aside aria-label="Graph tools">
    <div class="search-wrap">
      <input id="search" type="search" placeholder="Search nodes..." autocomplete="off">
    </div>
    <div class="info-panel">
      <h3 class="panel-head">Node Info</h3>
      <div id="node-details">
        <p class="empty">Click a node to inspect its source, id, and neighbors.</p>
      </div>
    </div>
    <div class="legend-wrap">
      <h3 class="panel-head">Communities</h3>
      <div class="legend" id="legend" aria-label="Community color key"></div>
    </div>
)html";
  output += "    <div class=\"stats\" id=\"stats\">";
  output += std::to_string(graph.nodes.size());
  output += " nodes &middot; ";
  output += std::to_string(graph.edges.size());
  output += " edges</div>\n";
  output += R"html(  </aside>
</div>
<script>
)html";
  output += "const graphData = ";
  append_json_for_script(output, to_node_link_json(graph));
  output += ";\n";
  output += R"html(
const canvas = document.getElementById("graph-canvas");
const ctx = canvas.getContext("2d");
const search = document.getElementById("search");
const details = document.getElementById("node-details");
const fitButton = document.getElementById("fit-view");
const resetButton = document.getElementById("reset-view");
const themeToggle = document.getElementById("theme-toggle");
const nodes = graphData.nodes.map((node, index) => ({...node, index}));
const links = graphData.links || [];
const nodeById = new Map(nodes.map(node => [node.id, node]));
const palette = {
  class: "#4E79A7",
  function: "#59A14F",
  other: "#B07AA1",
  edge: "#5a6282",
  text: "#ffffff",
  muted: "#888888",
  focus: "#EDC948",
  nodeStroke: "#0f0f1a"
};

// The canvas cannot inherit CSS, so pull the theme-dependent colors from the
// document's CSS variables into the palette whenever the theme changes.
function refreshTheme() {
  const css = getComputedStyle(document.documentElement);
  palette.text = (css.getPropertyValue("--graph-text").trim()) || palette.text;
  palette.edge = (css.getPropertyValue("--graph-edge").trim()) || palette.edge;
  palette.muted = (css.getPropertyValue("--muted").trim()) || palette.muted;
  palette.nodeStroke = (css.getPropertyValue("--node-stroke").trim()) || palette.nodeStroke;
}
let selectedId = "";
let hoverId = "";
let searchTerm = "";
let transform = {x: 0, y: 0, scale: 1};
let dragging = false;
let draggingNodeId = "";
let dragStart = {x: 0, y: 0, tx: 0, ty: 0, nodeX: 0, nodeY: 0};
let layoutReady = false;
const outgoing = new Map();
const incoming = new Map();
const degree = new Map();
for (const node of nodes) degree.set(node.id, 0);
for (const link of links) {
  if (!outgoing.has(link.source)) outgoing.set(link.source, []);
  if (!incoming.has(link.target)) incoming.set(link.target, []);
  outgoing.get(link.source).push(link);
  incoming.get(link.target).push(link);
  degree.set(link.source, (degree.get(link.source) || 0) + 1);
  degree.set(link.target, (degree.get(link.target) || 0) + 1);
}

// Group nodes by community so the layout can pull each cluster together and
// seed members near a shared anchor. Communities are ordered deterministically
// (by first appearance) so anchor placement is stable across loads.
const communityOrder = [];
const communityIndex = new Map();
for (const node of nodes) {
  const community = communityFor(node) || ("__node_" + node.id);
  if (!communityIndex.has(community)) {
    communityIndex.set(community, communityOrder.length);
    communityOrder.push(community);
  }
  node.community = community;
}

// Bounded label budget: only the highest-degree nodes are labelled at rest, so
// an overview of a large graph stays legible instead of becoming a wall of
// text. Every other label is still reachable on hover/selection/search/zoom.
const LABEL_BUDGET = 24;
const labelBudget = new Set(
  [...nodes]
    .sort((a, b) => (degree.get(b.id) || 0) - (degree.get(a.id) || 0))
    .slice(0, LABEL_BUDGET)
    .map(node => node.id)
);

// Largest degree in the graph, so node sizing can be normalized against the
// busiest hub exactly like the Graphify viewer (radius 5..20 over [0, maxDeg]).
let maxDegree = 1;
for (const value of degree.values()) maxDegree = Math.max(maxDegree, value);

function radiusFor(node) {
  // Degree-normalized sizing in the spirit of Graphify's `size = 10 + 30*(deg/max)`,
  // but sqrt-scaled so a single dominant hub does not squash every other node to
  // the floor — this restores the clear hub-to-leaf size hierarchy the reference
  // shows. "god" hubs get a small extra bump.
  const deg = degree.get(node.id) || 0;
  const god = node && node.properties && String(node.properties.god_node) === "true";
  const base = 5 + 17 * Math.sqrt(deg / maxDegree);
  return Math.min(24, god ? base + 3 : base);
}

function colorFor(node) {
  // Color by community so clusters read as distinct regions, matching
  // Graphify's community-tinted layout. Falls back to node kind when a node has
  // no community assignment.
  const community = communityFor(node);
  if (community !== "") {
    let hash = 0;
    for (let i = 0; i < community.length; ++i) hash = (hash * 31 + community.charCodeAt(i)) | 0;
    return communityPalette[Math.abs(hash) % communityPalette.length];
  }
  const kind = String(node.type || node.kind || "").toLowerCase();
  if (kind.includes("class")) return palette.class;
  if (kind.includes("function") || kind.includes("method")) return palette.function;
  return palette.other;
}

// Tableau 10 — the exact community palette the Graphify viewer uses, so
// clusters read with the same colors across both tools.
const communityPalette = [
  "#4E79A7", "#F28E2B", "#E15759", "#76B7B2", "#59A14F",
  "#EDC948", "#B07AA1", "#FF9DA7", "#9C755F", "#BAB0AC"
];

function shortLabel(value) {
  const label = String(value || "");
  return label.length > 28 ? label.slice(0, 25) + "..." : label;
}

function escapeHtml(value) {
  return String(value || "").replace(/[&<>"']/g, ch => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "\"": "&quot;",
    "'": "&#39;"
  }[ch]));
}

function communityFor(node) {
  return node && node.properties ? String(node.properties.community || "") : "";
}

// Force-directed (Fruchterman-Reingold style) layout so connected nodes
// cluster and relationships become visible, instead of every node landing on
// one circular ring. Runs as a cooling simulation on animation frames.
let sim = {alpha: 0, k: 60, width: 720, height: 620};

// Deterministic per-node jitter so the same graph lays out the same way each
// load without depending on Math.random.
function seededUnit(seed) {
  const x = Math.sin(seed * 127.1 + 311.7) * 43758.5453;
  return x - Math.floor(x);
}

// True when every node carries a server-precomputed x/y (igraph layout stamped
// into node.properties by the C++ engine). When present we adopt those as final
// positions and skip the browser force simulation entirely, so a large graph
// renders near-instantly instead of paying an O(N^2) per-frame cool-down.
function hasEmbeddedLayout() {
  if (nodes.length === 0) return false;
  for (const node of nodes) {
    const props = node.properties;
    if (!props) return false;
    const x = Number(props.x);
    const y = Number(props.y);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return false;
  }
  return true;
}

function layout() {
  if (layoutReady) return;
  const box = canvas.getBoundingClientRect();
  sim.width = Math.max(box.width, 720);
  sim.height = Math.max(box.height, 620);
  const centerX = sim.width / 2;
  const centerY = sim.height / 2;
  // Ideal edge length scales with available area per node.
  sim.k = Math.max(34, Math.sqrt((sim.width * sim.height) / Math.max(nodes.length, 1)));
  // Precomputed layout: adopt server x/y verbatim (fitToScreen maps the layout's
  // arbitrary coordinate scale into the viewport), then leave the simulation
  // cold so nothing moves and settle time is effectively zero.
  if (hasEmbeddedLayout()) {
    for (const node of nodes) {
      node.x = Number(node.properties.x);
      node.y = Number(node.properties.y);
    }
    sim.alpha = 0;
    layoutReady = true;
    return;
  }
  // Seed each community around a ring so distinct clusters start in distinct
  // regions; members land near their community anchor with seeded jitter. The
  // centroid force in simulationTick then keeps them together while edges shape
  // the interior. This makes the computed community structure spatially visible
  // instead of relaxing into one uniform frame-filling cloud.
  const communityCount = Math.max(communityOrder.length, 1);
  // Seed communities on a wide ellipse that tracks the (typically 16:9) canvas
  // aspect, so clusters spread into 2D islands that fill the frame instead of
  // collapsing into a tall vertical spine. Weak global gravity (simulationTick)
  // then keeps them on-screen without pulling them back into a line.
  const ringRadiusX = sim.width * 0.34;
  const ringRadiusY = sim.height * 0.34;
  for (const node of nodes) {
    const slot = communityIndex.get(node.community) || 0;
    const angle = (slot / communityCount) * 2 * Math.PI;
    const anchorX = centerX + ringRadiusX * Math.cos(angle);
    const anchorY = centerY + ringRadiusY * Math.sin(angle);
    const spread = sim.k * 2.2;
    node.x = anchorX + (seededUnit(node.index + 1) - 0.5) * spread;
    node.y = anchorY + (seededUnit(node.index + 13) - 0.5) * spread;
  }
  sim.alpha = 1;
  layoutReady = true;
}

function simulationTick() {
  const k = sim.k;
  const centerX = sim.width / 2;
  const centerY = sim.height / 2;
  // Repulsion acts within a wide cutoff so distinct communities genuinely push
  // apart into separate regions, rather than the tight local cutoff that let
  // the whole graph relax into one uniform blob. The cutoff stays finite so the
  // per-tick cost is bounded (a future Barnes-Hut swap can drop it entirely).
  const cutoff = k * 8;
  const cutoffSq = cutoff * cutoff;
  for (const node of nodes) { node.dx = 0; node.dy = 0; }
  for (let i = 0; i < nodes.length; ++i) {
    const a = nodes[i];
    for (let j = i + 1; j < nodes.length; ++j) {
      const b = nodes[j];
      let dx = a.x - b.x;
      let dy = a.y - b.y;
      let distSq = dx * dx + dy * dy;
      if (distSq > cutoffSq) continue;
      if (distSq < 0.01) {
        dx = seededUnit(i * 31 + j) - 0.5;
        dy = seededUnit(j * 31 + i) - 0.5;
        distSq = dx * dx + dy * dy + 0.01;
      }
      const dist = Math.sqrt(distSq);
      const force = (k * k) / dist;
      const fx = (dx / dist) * force;
      const fy = (dy / dist) * force;
      a.dx += fx; a.dy += fy;
      b.dx -= fx; b.dy -= fy;
    }
  }
  // Attraction along edges (springs pulling connected nodes together).
  for (const link of links) {
    const source = nodeById.get(link.source);
    const target = nodeById.get(link.target);
    if (!source || !target) continue;
    const dx = target.x - source.x;
    const dy = target.y - source.y;
    const dist = Math.sqrt(dx * dx + dy * dy) || 0.01;
    const force = (dist * dist) / k;
    const fx = (dx / dist) * force;
    const fy = (dy / dist) * force;
    source.dx += fx; source.dy += fy;
    target.dx -= fx; target.dy -= fy;
  }
  // Community cohesion: pull each node toward its community's running centroid
  // (communityCentroid) so clusters stay tight and read as distinct regions,
  // making the computed community structure spatially visible.
  const communityCentroid = new Map();
  for (const node of nodes) {
    let acc = communityCentroid.get(node.community);
    if (!acc) { acc = {x: 0, y: 0, count: 0}; communityCentroid.set(node.community, acc); }
    acc.x += node.x; acc.y += node.y; acc.count += 1;
  }
  for (const acc of communityCentroid.values()) {
    acc.x /= acc.count;
    acc.y /= acc.count;
  }
  for (const node of nodes) {
    const acc = communityCentroid.get(node.community);
    if (acc) {
      node.dx += (acc.x - node.x) * 0.18;
      node.dy += (acc.y - node.y) * 0.18;
    }
  }
  // Very weak global gravity keeps the layout on-screen without pulling the
  // spread-out community islands back into a central spine.
  for (const node of nodes) {
    node.dx += (centerX - node.x) * 0.006;
    node.dy += (centerY - node.y) * 0.006;
  }
  // Move each node by its net displacement, limited by the cooling temperature.
  const maxStep = k * sim.alpha;
  for (const node of nodes) {
    if (node.id === draggingNodeId) continue;
    const disp = Math.sqrt(node.dx * node.dx + node.dy * node.dy);
    if (disp < 0.001) continue;
    const step = Math.min(disp, maxStep);
    node.x += (node.dx / disp) * step;
    node.y += (node.dy / disp) * step;
    // No hard wall clamp: nodes relax freely in open world space and stay
    // bounded by gravity + community cohesion. Clamping to the viewport box
    // pinned outward-pushed nodes into a dense rectangular border. Pan, zoom,
    // and Fit to screen handle navigation instead.
  }
  sim.alpha *= 0.985;
}

function resizeCanvas() {
  const box = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(box.width, 720);
  const height = Math.max(box.height, 620);
  canvas.width = Math.floor(width * ratio);
  canvas.height = Math.floor(height * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

function relatedIds(id) {
  const related = new Set([id]);
  for (const link of outgoing.get(id) || []) related.add(link.target);
  for (const link of incoming.get(id) || []) related.add(link.source);
  return related;
}

function highlightIdsFor(id) {
  if (!id) return new Set();
  const node = nodeById.get(id);
  const highlighted = relatedIds(id);
  const community = communityFor(node);
  if (community) {
    for (const candidate of nodes) {
      if (communityFor(candidate) === community) {
        highlighted.add(candidate.id);
      }
    }
  }
  return highlighted;
}

function matchesSearch(node) {
  if (!searchTerm) return true;
  return `${node.label || ""} ${node.id || ""} ${node.source_file || ""}`.toLowerCase().includes(searchTerm);
}

function nodeIsDim(node) {
  if (searchTerm && !matchesSearch(node)) return true;
  const activeId = hoverId || selectedId;
  if (!activeId) return false;
  return !highlightIdsFor(activeId).has(node.id);
}

function screenPoint(worldX, worldY) {
  return {
    x: worldX * transform.scale + transform.x,
    y: worldY * transform.scale + transform.y
  };
}

function worldPoint(screenX, screenY) {
  return {
    x: (screenX - transform.x) / transform.scale,
    y: (screenY - transform.y) / transform.scale
  };
}

function viewportBounds(box) {
  const margin = 48;
  return {
    left: -margin,
    top: -margin,
    right: box.width + margin,
    bottom: box.height + margin
  };
}

function visibleCircle(point, radius, bounds) {
  return point.x + radius >= bounds.left &&
    point.x - radius <= bounds.right &&
    point.y + radius >= bounds.top &&
    point.y - radius <= bounds.bottom;
}

function visibleSegment(a, b, radius, bounds) {
  return Math.max(a.x, b.x) + radius >= bounds.left &&
    Math.min(a.x, b.x) - radius <= bounds.right &&
    Math.max(a.y, b.y) + radius >= bounds.top &&
    Math.min(a.y, b.y) - radius <= bounds.bottom;
}

function draw() {
  const box = canvas.getBoundingClientRect();
  const bounds = viewportBounds(box);
  const activeId = hoverId || selectedId;
  const highlighted = highlightIdsFor(activeId);
  ctx.clearRect(0, 0, box.width, box.height);
  ctx.save();
  ctx.translate(transform.x, transform.y);
  ctx.scale(transform.scale, transform.scale);

  for (const link of links) {
    const source = nodeById.get(link.source);
    const target = nodeById.get(link.target);
    if (!source || !target) continue;
    const sourcePoint = screenPoint(source.x, source.y);
    const targetPoint = screenPoint(target.x, target.y);
    const activeEdge = activeId && highlighted.has(source.id) && highlighted.has(target.id);
    const edgeLength = Math.hypot(targetPoint.x - sourcePoint.x, targetPoint.y - sourcePoint.y);
    if (!activeEdge && edgeLength < 0.75) continue;
    const sourceRadius = radiusFor(source);
    const targetRadius = radiusFor(target);
    const edgeRadius = (Math.max(sourceRadius, targetRadius) + (activeEdge ? 9 : 7) + 2) * transform.scale;
    if (!visibleSegment(sourcePoint, targetPoint, edgeRadius, bounds)) continue;
    const dim = nodeIsDim(source) || nodeIsDim(target);
    // Edges inherit the source node's community color at low opacity, the same
    // "inherit from" tint vis.js gives the Graphify viewer, so clusters read as
    // cohesive colored regions instead of a uniform gray mesh.
    const edgeColor = activeEdge ? palette.focus : colorFor(source);
    ctx.globalAlpha = dim ? 0.08 : activeEdge ? 0.9 : 0.45;
    ctx.strokeStyle = edgeColor;
    ctx.lineWidth = (activeEdge ? 2.2 : 1.2) / transform.scale;
    // Stop the line at the target node's rim so the arrowhead sits cleanly.
    const dx = target.x - source.x;
    const dy = target.y - source.y;
    const dist = Math.sqrt(dx * dx + dy * dy) || 1;
    const ux = dx / dist;
    const uy = dy / dist;
    const targetR = targetRadius;
    const tipX = target.x - ux * (targetR + 1.5);
    const tipY = target.y - uy * (targetR + 1.5);
    ctx.beginPath();
    ctx.moveTo(source.x + ux * sourceRadius, source.y + uy * sourceRadius);
    ctx.lineTo(tipX, tipY);
    ctx.stroke();
    // Arrowhead conveys edge direction (CALLS, IMPORTS, ...).
    const head = (activeEdge ? 9 : 7) / transform.scale;
    const ang = Math.atan2(uy, ux);
    ctx.beginPath();
    ctx.moveTo(tipX, tipY);
    ctx.lineTo(tipX - head * Math.cos(ang - 0.4), tipY - head * Math.sin(ang - 0.4));
    ctx.lineTo(tipX - head * Math.cos(ang + 0.4), tipY - head * Math.sin(ang + 0.4));
    ctx.closePath();
    ctx.fillStyle = edgeColor;
    ctx.fill();
    // Relation label only on the active node's edges, to avoid clutter.
    if (activeEdge && link.relation) {
      ctx.globalAlpha = 0.9;
      ctx.fillStyle = palette.muted;
      ctx.font = (10 / transform.scale) + "px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif";
      ctx.fillText(String(link.relation), (source.x + target.x) / 2 + 4, (source.y + target.y) / 2);
    }
  }

  ctx.font = "11px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif";
  ctx.textBaseline = "middle";
  ctx.textAlign = "center";
  for (const node of nodes) {
    const selected = node.id === selectedId;
    const hovered = node.id === hoverId;
    const inHighlight = activeId && highlighted.has(node.id);
    const searchMatch = searchTerm && matchesSearch(node);
    const activeCommunity = activeId && highlighted.has(node.id) && communityFor(node) === communityFor(nodeById.get(activeId));

    const r = radiusFor(node) + (hovered ? 2 : 0);
    const point = screenPoint(node.x, node.y);
    const visibleRadius = (r + (activeCommunity ? 11 : 3)) * transform.scale;
    if (!visibleCircle(point, visibleRadius, bounds)) continue;
    if (!selected && !hovered && !inHighlight && !searchMatch && r * transform.scale < 0.5) continue;

    const dim = nodeIsDim(node);
    ctx.globalAlpha = dim ? 0.18 : 1;

    if (activeCommunity) {
      ctx.beginPath();
      ctx.fillStyle = palette.focus;
      ctx.globalAlpha = dim ? 0.08 : 0.14;
      ctx.arc(node.x, node.y, r + 11, 0, 2 * Math.PI);
      ctx.fill();
      ctx.globalAlpha = dim ? 0.18 : 1;
    }

    ctx.beginPath();
    ctx.fillStyle = colorFor(node);
    ctx.strokeStyle = selected ? palette.focus : palette.nodeStroke;
    ctx.lineWidth = selected ? 4 / transform.scale : 2.5 / transform.scale;
    ctx.arc(node.x, node.y, r, 0, 2 * Math.PI);
    ctx.fill();
    ctx.stroke();

    // Label only the bounded top-by-degree budget at rest; reveal everything
    // else progressively on hover, selection, active highlight, search match,
    // or when zoomed in, so the overview stays legible but no label is ever
    // permanently hidden.
    const zoomedIn = transform.scale >= 1.6;
    const labelled = !dim && (
      labelBudget.has(node.id) || hovered || selected || inHighlight ||
      zoomedIn || searchMatch);
    if (labelled) {
      ctx.fillStyle = palette.text;
      ctx.globalAlpha = dim ? 0.22 : 0.95;
      // Centered just below the node, matching the Graphify viewer's labels.
      ctx.fillText(shortLabel(node.label || node.id), node.x, node.y + r + 9);
    }
  }
  ctx.restore();
  ctx.globalAlpha = 1;
  ctx.textAlign = "left";
}

let animationHandle = 0;
// Auto-fit the whole graph into view once the initial layout settles, since
// the unclamped layout spreads beyond the viewport. Cancelled the moment the
// user pans, zooms, or selects, so we never yank a view they are using.
let autoFitPending = true;
function runSimulation() {
  if (animationHandle) cancelAnimationFrame(animationHandle);
  // Precomputed layout (or a fully cooled sim): positions are final, so skip the
  // O(N^2) simulationTick entirely and just fit + draw a single static frame.
  // This is the fast path that drops large-graph settle time toward zero.
  if (sim.alpha <= 0.02) {
    if (autoFitPending) { fitToScreen(); autoFitPending = false; }
    draw();
    animationHandle = 0;
    return;
  }
  const tick = () => {
    // A few ticks per frame settle the layout quickly without a visible crawl.
    for (let i = 0; i < 2; ++i) simulationTick();
    if (autoFitPending) fitToScreen();
    draw();
    if (sim.alpha > 0.02) {
      animationHandle = requestAnimationFrame(tick);
    } else {
      if (autoFitPending) { fitToScreen(); autoFitPending = false; }
      animationHandle = 0;
    }
  };
  animationHandle = requestAnimationFrame(tick);
}

function render() {
  resizeCanvas();
  layout();
  runSimulation();
}

function hitNode(event) {
  const rect = canvas.getBoundingClientRect();
  const point = worldPoint(event.clientX - rect.left, event.clientY - rect.top);
  for (let index = nodes.length - 1; index >= 0; --index) {
    const node = nodes[index];
    const dx = point.x - node.x;
    const dy = point.y - node.y;
    if (Math.sqrt(dx * dx + dy * dy) <= radiusFor(node) + 4) {
      return node;
    }
  }
  return null;
}

function selectNode(id) {
  const node = nodeById.get(id);
  if (!node) return;
  selectedId = id;
  const outgoingLinks = outgoing.get(id) || [];
  const incomingLinks = incoming.get(id) || [];
  const community = communityFor(node) || "none";
  details.innerHTML = `
    <h2 class="detail-title">${escapeHtml(node.label || node.id)}</h2>
    <span class="detail-kind">${escapeHtml(node.type || node.kind || "node")}</span>
    <div class="field"><strong>Source</strong><code>${escapeHtml(node.source_file || "unknown")}</code></div>
    <div class="field"><strong>ID</strong><code>${escapeHtml(node.id)}</code></div>
    <div class="field"><strong>Community</strong><code>${escapeHtml(community)}</code></div>
    <div class="field"><strong>Outgoing</strong><code>${outgoingLinks.length}</code></div>
    <div class="field"><strong>Incoming</strong><code>${incomingLinks.length}</code></div>
  `;
  draw();
}

function applySearch() {
  searchTerm = search.value.trim().toLowerCase();
  draw();
}

// Build the legend as a live color key: one row per detected community (its
// actual swatch color, a representative label, and node count), ordered by
// size, plus the size hint. Clicking a row focuses that community's hub.
function buildLegend() {
  const legend = document.getElementById("legend");
  if (!legend) return;
  const groups = new Map();
  for (const node of nodes) {
    const community = communityFor(node);
    if (!community) continue;
    let group = groups.get(community);
    if (!group) { group = {count: 0, rep: node}; groups.set(community, group); }
    group.count += 1;
    if ((degree.get(node.id) || 0) > (degree.get(group.rep.id) || 0)) group.rep = node;
  }
  const ordered = [...groups.values()].sort((a, b) => b.count - a.count);
  // The "Communities" heading lives in the sidebar; rows are dot + label + count
  // like the Graphify legend, ordered by size.
  let html = '';
  for (const group of ordered) {
    const rep = group.rep;
    html += '<div class="row legend-item" data-node="' + escapeHtml(rep.id) + '" title="' +
      escapeHtml(rep.label || rep.id) + '">' +
      '<span class="dot" style="background:' + colorFor(rep) + '"></span>' +
      '<span class="lbl">' + escapeHtml(shortLabel(rep.label || rep.id)) + '</span>' +
      '<span class="count">' + group.count + '</span></div>';
  }
  html += '<div class="row foot"><span class="dot" style="background:var(--edge)"></span>Bigger = more connected</div>';
  legend.innerHTML = html;
  // Fold the community count into the stats footer, matching Graphify's
  // "N nodes · M edges · K communities" summary line.
  const statsEl = document.getElementById("stats");
  if (statsEl && ordered.length) {
    statsEl.innerHTML = nodes.length + " nodes · " + links.length + " edges · " + ordered.length + " communities";
  }
  for (const item of legend.querySelectorAll(".legend-item")) {
    item.style.cursor = "pointer";
    item.addEventListener("click", () => {
      autoFitPending = false;
      selectNode(item.getAttribute("data-node"));
    });
  }
}

// Returning to the unfocused full graph without reloading: clear the selection
// and highlight and restore the empty details panel.
function clearSelection() {
  selectedId = "";
  hoverId = "";
  details.innerHTML = '<p class="empty">Select a node to inspect its source, id, and neighbors.</p>';
  draw();
}

// Reset view: clear selection and return the pan/zoom transform to identity.
function resetView() {
  transform = {x: 0, y: 0, scale: 1};
  clearSelection();
}

// Fit to screen: center and scale the whole graph's bounding box into the
// viewport, the recenter affordance the view otherwise lacks.
function fitToScreen() {
  if (!nodes.length) return;
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const node of nodes) {
    const r = radiusFor(node);
    minX = Math.min(minX, node.x - r);
    minY = Math.min(minY, node.y - r);
    maxX = Math.max(maxX, node.x + r);
    maxY = Math.max(maxY, node.y + r);
  }
  const box = canvas.getBoundingClientRect();
  const margin = 40;
  const spanX = Math.max(maxX - minX, 1);
  const spanY = Math.max(maxY - minY, 1);
  const scale = Math.max(0.25, Math.min(4,
    Math.min((box.width - margin * 2) / spanX, (box.height - margin * 2) / spanY)));
  transform.scale = scale;
  transform.x = box.width / 2 - ((minX + maxX) / 2) * scale;
  transform.y = box.height / 2 - ((minY + maxY) / 2) * scale;
  draw();
}

canvas.addEventListener("pointerdown", event => {
  autoFitPending = false;
  const node = hitNode(event);
  if (node) {
    selectNode(node.id);
    dragging = true;
    draggingNodeId = node.id;
    dragStart = {x: event.clientX, y: event.clientY, tx: transform.x, ty: transform.y, nodeX: node.x, nodeY: node.y};
    canvas.setPointerCapture(event.pointerId);
    // Reheat the simulation so neighbors follow the dragged node.
    sim.alpha = Math.max(sim.alpha, 0.35);
    if (!animationHandle) runSimulation();
    return;
  }
  // Empty-canvas press clears any active selection before starting a pan, so
  // the user can always get back to the unfocused full graph.
  if (selectedId) clearSelection();
  dragging = true;
  draggingNodeId = "";
  dragStart = {x: event.clientX, y: event.clientY, tx: transform.x, ty: transform.y};
  canvas.setPointerCapture(event.pointerId);
});

canvas.addEventListener("pointermove", event => {
  if (dragging && draggingNodeId) {
    const node = nodeById.get(draggingNodeId);
    if (node) {
      const rect = canvas.getBoundingClientRect();
      const pointer = worldPoint(event.clientX - rect.left, event.clientY - rect.top);
      const start = worldPoint(dragStart.x - rect.left, dragStart.y - rect.top);
      node.x = dragStart.nodeX + pointer.x - start.x;
      node.y = dragStart.nodeY + pointer.y - start.y;
      hoverId = draggingNodeId;
    }
  } else if (dragging) {
    transform.x = dragStart.tx + event.clientX - dragStart.x;
    transform.y = dragStart.ty + event.clientY - dragStart.y;
  } else {
    const node = hitNode(event);
    hoverId = node ? node.id : "";
  }
  draw();
});

canvas.addEventListener("pointerup", event => {
  dragging = false;
  draggingNodeId = "";
  canvas.releasePointerCapture(event.pointerId);
});

canvas.addEventListener("pointerleave", () => {
  if (!dragging) {
    hoverId = "";
    draw();
  }
});

canvas.addEventListener("wheel", event => {
  event.preventDefault();
  autoFitPending = false;
  const rect = canvas.getBoundingClientRect();
  const before = worldPoint(event.clientX - rect.left, event.clientY - rect.top);
  const factor = event.deltaY < 0 ? 1.12 : 0.89;
  transform.scale = Math.max(0.25, Math.min(4, transform.scale * factor));
  const after = screenPoint(before.x, before.y);
  transform.x += event.clientX - rect.left - after.x;
  transform.y += event.clientY - rect.top - after.y;
  draw();
}, {passive: false});

// Light/dark theme: an explicit data-theme attribute overrides the OS
// preference once the user picks. The canvas palette is refreshed from CSS so
// node/edge/label colors follow the theme.
function effectiveDark() {
  // Dark is the unconditional default (matching the dark-only Graphify viewer);
  // light is reachable only by an explicit toggle.
  return document.documentElement.getAttribute("data-theme") !== "light";
}
function syncThemeButton() {
  const dark = effectiveDark();
  themeToggle.textContent = dark ? "Light" : "Dark";
  themeToggle.setAttribute("aria-pressed", dark ? "true" : "false");
}
function toggleTheme() {
  document.documentElement.setAttribute("data-theme", effectiveDark() ? "light" : "dark");
  refreshTheme();
  syncThemeButton();
  draw();
}
themeToggle.addEventListener("click", toggleTheme);

search.addEventListener("input", applySearch);
fitButton.addEventListener("click", fitToScreen);
resetButton.addEventListener("click", resetView);
// Escape clears the current selection and highlight, returning to the full
// graph without a reload.
window.addEventListener("keydown", event => {
  if (event.key === "Escape") {
    clearSelection();
  }
});
window.addEventListener("resize", render);
refreshTheme();
syncThemeButton();
buildLegend();
render();
</script>
</body>
</html>)html";
  return output;
}

std::string export_graph_svg(const GraphSnapshot& graph) {
  const auto count = std::max<std::size_t>(graph.nodes.size(), 1);
  constexpr double center_x = 240.0;
  constexpr double center_y = 180.0;
  constexpr double radius = 120.0;
  constexpr double pi = 3.14159265358979323846;

  std::vector<std::pair<double, double>> positions;
  positions.reserve(graph.nodes.size());
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto angle = (2.0 * pi * static_cast<double>(i)) / static_cast<double>(count);
    positions.emplace_back(center_x + radius * std::cos(angle), center_y + radius * std::sin(angle));
  }

  const auto index = node_index(graph);
  std::ostringstream output;
  output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 480 360\">";
  output << "<rect width=\"480\" height=\"360\" fill=\"#ffffff\"/>";
  for (const auto& edge : graph.edges) {
    const auto source = index.find(edge.source);
    const auto target = index.find(edge.target);
    if (source == index.end() || target == index.end()) {
      continue;
    }
    const auto [x1, y1] = positions[source->second];
    const auto [x2, y2] = positions[target->second];
    output << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
           << "\" stroke=\"#8a8f98\" stroke-width=\"1.5\"/>";
  }
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto [x, y] = positions[i];
    output << "<circle cx=\"" << x << "\" cy=\"" << y << "\" r=\"16\" fill=\"#2563eb\"/>";
    output << "<text x=\"" << x << "\" y=\"" << (y + 30.0)
           << "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#111827\">" << html_escape(graph.nodes[i].label)
           << "</text>";
  }
  output << "</svg>";
  return output.str();
}

std::string export_obsidian_markdown(const GraphSnapshot& graph) {
  // Index outgoing edge targets by source in a single pass over edges. Iterating
  // edges in order preserves the per-node link ordering of the original
  // O(nodes * edges) scan, so the output stays byte-identical.
  std::unordered_map<std::string, std::vector<std::string>> targets_by_source;
  targets_by_source.reserve(graph.nodes.size());
  for (const auto& edge : graph.edges) {
    targets_by_source[edge.source].push_back(edge.target);
  }

  std::ostringstream output;
  for (const auto& node : graph.nodes) {
    output << "# " << node.label << "\n\n";
    output << "- id: `" << node.id << "`\n";
    output << "- kind: `" << node.kind << "`\n";
    output << "- source: `" << node.source_file << "`\n";
    output << "- links:";
    const auto it = targets_by_source.find(node.id);
    if (it == targets_by_source.end() || it->second.empty()) {
      output << " none";
    } else {
      for (const auto& target : it->second) {
        output << " [[" << target << "]]";
      }
    }
    output << "\n\n";
  }
  return output.str();
}

std::string export_neo4j_cypher(const GraphSnapshot& graph) {
  std::ostringstream output;
  for (const auto& node : graph.nodes) {
    output << "MERGE (n:Symbol {id: '" << cypher_escape(node.id) << "'}) "
           << "SET n.label = '" << cypher_escape(node.label) << "', n.kind = '" << cypher_escape(node.kind)
           << "', n.source_file = '" << cypher_escape(node.source_file) << "';\n";
  }
  for (const auto& edge : graph.edges) {
    output << "MATCH (a:Symbol {id: '" << cypher_escape(edge.source) << "'}), "
           << "(b:Symbol {id: '" << cypher_escape(edge.target) << "'}) "
           << "MERGE (a)-[:`" << cypher_escape(edge.relation) << "`]->(b);\n";
  }
  return output.str();
}

std::string export_call_flow_html(const GraphSnapshot& graph) {
  std::ostringstream output;
  output << "<!doctype html><html><head><meta charset=\"utf-8\"><title>call flow</title></head><body>";
  output << "<h1>Call Flow</h1><ol>";
  for (const auto& edge : graph.edges) {
    if (edge.relation == "CALLS") {
      output << "<li>" << html_escape(edge.source) << " calls " << html_escape(edge.target) << "</li>";
    }
  }
  output << "</ol></body></html>";
  return output.str();
}

}  // namespace cgraph
