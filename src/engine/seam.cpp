#include "cgraph/seam.hpp"

#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgraph {

bool is_seam_directory(const std::filesystem::path& root) {
  std::error_code ec;
  return std::filesystem::exists(root / kSeamMarkerFile, ec);
}

namespace {

// A consumer graph loaded just enough to resolve (file, line) anchors to real
// nodes. Purpose-built rather than reusing the daemon's node-link parser, which is
// file-local to daemon_lifecycle.cpp; exporting it would refactor the daemon load
// path for fields the seam never needs (edges, build_state). We read only what
// anchor resolution requires: id, label, source_file, kind, and the line span.
struct SeamNode {
  std::string id;
  std::string label;
  std::string source_file;
  std::string kind;
  std::uint32_t start_line = 0;
  std::uint32_t end_line = 0;
  bool has_span = false;
  std::map<std::string, std::string> properties;  // string-valued properties (endpoint method/path/served)
};

struct SeamEdge {
  std::string source;
  std::string target;
  std::string relation;
};

class SeamGraph {
 public:
  SeamGraph() = default;

  [[nodiscard]] static std::optional<SeamGraph> load(
      const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
      error = "graph not found: " + path.generic_string();
      return std::nullopt;
    }
    nlohmann::json data;
    try {
      input >> data;
    } catch (const nlohmann::json::exception& ex) {
      error = "graph is malformed JSON: " + path.generic_string() + " (" + ex.what() + ")";
      return std::nullopt;
    }
    SeamGraph graph;
    for (const auto& node : data.value("nodes", nlohmann::json::array())) {
      SeamNode sn;
      sn.id = node.value("id", std::string{});
      sn.label = node.value("label", std::string{});
      sn.source_file = node.value("source_file", std::string{});
      // Node-link exports the kind under "type"; fall back to "kind".
      sn.kind = node.value("type", node.value("kind", std::string{}));
      if (const auto loc = node.find("source_location"); loc != node.end() && loc->is_object()) {
        sn.start_line = loc->value("start_line", 0U);
        sn.end_line = loc->value("end_line", 0U);
        sn.has_span = true;
      }
      if (const auto props = node.find("properties"); props != node.end() && props->is_object()) {
        for (const auto& [key, value] : props->items()) {
          if (value.is_string()) {
            sn.properties.emplace(key, value.get<std::string>());
          }
        }
      }
      graph.nodes_.push_back(std::move(sn));
    }
    // Discovery reads the contract edges a graph carries (`handled_by`,
    // `CONSUMES`); anchor resolution never needed them.
    for (const auto& link : data.value("links", nlohmann::json::array())) {
      graph.edges_.push_back(SeamEdge{
          .source = link.value("source", std::string{}),
          .target = link.value("target", std::string{}),
          .relation = link.value("relation", std::string{}),
      });
    }
    return graph;
  }

  [[nodiscard]] const std::vector<SeamNode>& nodes() const { return nodes_; }
  [[nodiscard]] const std::vector<SeamEdge>& edges() const { return edges_; }
  [[nodiscard]] const SeamNode* find(const std::string& id) const {
    for (const auto& node : nodes_) {
      if (node.id == id) {
        return &node;
      }
    }
    return nullptr;
  }

  // The smallest-span non-file node whose source_file ends with `file_suffix` and
  // whose line span contains `line`. nullptr when nothing matches (caller fails loud).
  [[nodiscard]] const SeamNode* resolve(const std::string& file_suffix, std::uint32_t line) const {
    const SeamNode* best = nullptr;
    std::uint32_t best_span = 0;
    for (const auto& node : nodes_) {
      if (node.kind == "file" || !node.has_span || node.source_file.empty()) {
        continue;
      }
      if (!ends_with(node.source_file, file_suffix)) {
        continue;
      }
      if (line < node.start_line || line > node.end_line) {
        continue;
      }
      const std::uint32_t span = node.end_line - node.start_line;
      if (best == nullptr || span < best_span) {
        best = &node;
        best_span = span;
      }
    }
    return best;
  }

 private:
  [[nodiscard]] static bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::vector<SeamNode> nodes_;
  std::vector<SeamEdge> edges_;
};

[[nodiscard]] std::string endpoint_id(const std::string& provider, const std::string& method,
                                      const std::string& path) {
  return "endpoint:" + provider + ":" + method + " " + path;
}

[[nodiscard]] std::string schema_id(const std::string& provider, const std::string& api_version,
                                    const std::string& name) {
  return "schema:" + provider + ":" + api_version + ":" + name;
}

[[nodiscard]] std::string service_id(const std::string& name) { return "service:" + name; }

[[nodiscard]] std::string join_csv(const nlohmann::json& array, const std::string& suffix = "") {
  std::string out;
  for (const auto& item : array) {
    if (!out.empty()) {
      out += ",";
    }
    out += item.is_string() ? item.get<std::string>() : item.dump();
    out += suffix;
  }
  return out;
}

// Append a hard error and return false so the caller bails (fail loud, emit nothing).
bool fail(SeamResult& result, std::string message) {
  result.ok = false;
  result.errors.push_back(std::move(message));
  return false;
}

[[nodiscard]] bool require_fields(const nlohmann::json& obj, std::initializer_list<const char*> keys,
                                  const std::string& where, SeamResult& result) {
  for (const auto* key : keys) {
    if (!obj.contains(key)) {
      return fail(result, where + " is missing required field '" + key + "'");
    }
  }
  return true;
}

// The cluster a seam contract node belongs to: a service is its own cluster; an
// endpoint/schema clusters with its provider.
[[nodiscard]] std::string seam_community(const Node& node) {
  if (node.id.starts_with("service:")) {
    return node.label.empty() ? node.id.substr(8) : node.label;
  }
  if (node.id.starts_with("endpoint:")) {
    if (const auto it = node.properties.find("provider"); it != node.properties.end()) {
      return it->second;
    }
  }
  if (node.id.starts_with("schema:")) {
    // schema:<provider>:<api_version>:<name> -> <provider>
    const auto first = node.id.find(':');
    const auto second = node.id.find(':', first + 1);
    if (first != std::string::npos && second != std::string::npos) {
      return node.id.substr(first + 1, second - first - 1);
    }
  }
  return "";
}

}  // namespace

SeamFuseResult fuse_seam(const Fragment& seam,
                         const std::vector<std::pair<std::string, GraphSnapshot>>& services) {
  SeamFuseResult result;
  result.ok = true;

  std::vector<Node> nodes;
  std::unordered_map<std::string, std::size_t> index;  // id -> position in `nodes`
  auto put = [&](Node node, bool authoritative) {
    if (const auto it = index.find(node.id); it != index.end()) {
      if (authoritative) {
        nodes[it->second] = std::move(node);  // real service node wins over a seam placeholder
      }
      return;
    }
    index.emplace(node.id, nodes.size());
    nodes.push_back(std::move(node));
  };

  std::vector<Edge> edges;
  std::unordered_set<std::string> seen_edges;
  auto add_edge = [&](const Edge& edge) {
    const auto key = edge.source + "\x1f" + edge.target + "\x1f" + edge.relation;
    if (seen_edges.insert(key).second) {
      edges.push_back({.source = edge.source, .target = edge.target, .relation = edge.relation});
    }
  };

  // 1. Service code graphs: one community per service; real service nodes are authoritative.
  for (const auto& [name, graph] : services) {
    for (const auto& node : graph.nodes) {
      Node tagged = node;
      tagged.properties["community"] = name;
      tagged.properties.try_emplace("service", name);
      put(std::move(tagged), /*authoritative=*/true);
    }
    for (const auto& edge : graph.edges) {
      add_edge(edge);
    }
  }

  // 2. Seam contract nodes cluster with their service/provider; shadow code-refs
  // are dropped (the real service node already carries that id and neighborhood).
  for (const auto& node : seam.nodes) {
    if (node.kind == "code-ref") {
      continue;
    }
    Node tagged = node;
    tagged.properties["community"] = seam_community(node);
    put(std::move(tagged), /*authoritative=*/false);
  }
  for (const auto& edge : seam.edges) {
    add_edge(edge);
  }

  // 3. Fail loud: every edge endpoint must resolve to a fused node (else a service
  // graph was not supplied) -- never render a dangling picture.
  std::vector<std::string> missing;
  for (const auto& edge : edges) {
    if (!index.contains(edge.source)) {
      missing.push_back(edge.relation + ": source=" + edge.source);
    }
    if (!index.contains(edge.target)) {
      missing.push_back(edge.relation + ": target=" + edge.target);
    }
  }
  if (!missing.empty()) {
    result.ok = false;
    result.errors.push_back(
        std::to_string(missing.size()) +
        " edge endpoint(s) missing from the fused node set (supply the owning service graph via "
        "--graph): " +
        missing.front());
    return result;
  }

  result.graph.nodes = std::move(nodes);
  result.graph.edges = std::move(edges);
  result.graph.build_state = BuildState::DeterministicReady;
  return result;
}

SeamResult generate_seam(const nlohmann::json& spec,
                         const std::unordered_map<std::string, std::filesystem::path>& graph_paths) {
  SeamResult result;
  result.ok = true;

  if (!require_fields(spec, {"provider", "api_version", "services", "schemas", "endpoints",
                             "consumes", "mirrors"},
                      "seam spec", result)) {
    return result;
  }
  const auto provider = spec["provider"].get<std::string>();
  const auto api_version = spec["api_version"].get<std::string>();
  const auto default_errors = spec.value("error_codes", nlohmann::json::array());

  // Lazily load consumer graphs as anchors reference them, caching by name.
  std::unordered_map<std::string, SeamGraph> loaded;
  auto graph_for = [&](const std::string& name, const std::string& where,
                       const SeamGraph** out) -> bool {
    if (const auto it = loaded.find(name); it != loaded.end()) {
      *out = &it->second;
      return true;
    }
    const auto path_it = graph_paths.find(name);
    if (path_it == graph_paths.end()) {
      return fail(result, where + " references graph '" + name + "' but no such --graphs entry");
    }
    std::string error;
    auto graph = SeamGraph::load(path_it->second, error);
    if (!graph) {
      return fail(result, where + ": " + error);
    }
    *out = &(loaded.emplace(name, std::move(*graph)).first->second);
    return true;
  };

  // Insertion-ordered dedup: deterministic emission order (services, schemas,
  // endpoints, then resolved shadows in spec order) so regenerating is byte-stable.
  std::vector<Node> nodes;
  std::unordered_set<std::string> seen;
  std::vector<Edge> edges;
  auto add_node = [&](Node node) {
    if (seen.insert(node.id).second) {
      nodes.push_back(std::move(node));
    }
  };
  auto has_node = [&](const std::string& id) { return seen.contains(id); };

  // 1. service nodes
  for (const auto& service : spec["services"]) {
    if (!require_fields(service, {"name"}, "service", result)) {
      return result;
    }
    const auto name = service["name"].get<std::string>();
    Node node;
    node.id = service_id(name);
    node.label = name;
    node.kind = "service";
    node.properties["role"] = service.value("role", std::string{});
    node.properties["owned"] = service.value("owned", false) ? "true" : "false";
    for (const auto* key : {"status", "replacement", "graph"}) {
      if (const auto value = service.value(key, std::string{}); !value.empty()) {
        node.properties[key] = value;
      }
    }
    add_node(std::move(node));
  }

  // 2. schema nodes (keyed on contract coordinates; canonical file is a property)
  for (const auto& schema : spec["schemas"]) {
    if (!require_fields(schema, {"name", "canonical"}, "schema", result)) {
      return result;
    }
    const auto name = schema["name"].get<std::string>();
    Node node;
    node.id = schema_id(provider, api_version, name);
    node.label = name;
    node.kind = "schema";
    node.source_file = schema["canonical"].get<std::string>();
    node.properties["canonical"] = node.source_file;
    add_node(std::move(node));
  }

  // 3. endpoint nodes (+ SERVED_BY provider, + RESPONDS_WITH schema)
  for (const auto& endpoint : spec["endpoints"]) {
    if (!require_fields(endpoint, {"method", "path", "response_schema"}, "endpoint", result)) {
      return result;
    }
    const auto method = endpoint["method"].get<std::string>();
    const auto path = endpoint["path"].get<std::string>();
    const auto response_schema = endpoint["response_schema"].get<std::string>();
    const auto eid = endpoint_id(provider, method, path);
    Node node;
    node.id = eid;
    node.label = method + " " + path;
    node.kind = "endpoint";
    node.properties["provider"] = provider;
    node.properties["method"] = method;
    node.properties["path_template"] = path;
    node.properties["path_params"] = join_csv(endpoint.value("path_params", nlohmann::json::array()));
    node.properties["response_schema"] = response_schema;
    node.properties["error_codes"] =
        join_csv(endpoint.value("error_codes", default_errors));
    if (const auto query = endpoint.value("query_params", nlohmann::json::array()); !query.empty()) {
      node.properties["query_params"] = join_csv(query, "?");
    }
    add_node(std::move(node));
    edges.push_back({.source = eid, .target = service_id(provider), .relation = "SERVED_BY"});
    edges.push_back(
        {.source = eid, .target = schema_id(provider, api_version, response_schema),
         .relation = "RESPONDS_WITH"});
  }

  // A resolved anchor becomes a shadow code-ref node carrying the consumer node's
  // real id, so the cross-graph edge resolves locally while still pointing at the
  // consumer's own node for a later deep-dive.
  auto add_shadow = [&](const std::string& graph_name, const SeamNode& node) {
    Node shadow;
    shadow.id = node.id;
    shadow.label = node.label;
    shadow.kind = "code-ref";
    shadow.source_file = node.source_file;
    shadow.properties["service"] = graph_name;
    shadow.properties["symbol_kind"] = node.kind;
    shadow.properties["span"] =
        std::to_string(node.start_line) + "-" + std::to_string(node.end_line);
    add_node(std::move(shadow));
  };

  // 4. CONSUMES (service->endpoint) + CONSUMED_AT (endpoint->resolved consumer node)
  for (const auto& consume : spec["consumes"]) {
    if (!require_fields(consume, {"service", "method", "path", "call_site"}, "consumes", result)) {
      return result;
    }
    const auto method = consume["method"].get<std::string>();
    const auto path = consume["path"].get<std::string>();
    const auto eid = endpoint_id(provider, method, path);
    if (!has_node(eid)) {
      return (void)fail(result, "consumes references unknown endpoint: " + method + " " + path),
             result;
    }
    edges.push_back({.source = service_id(consume["service"].get<std::string>()), .target = eid,
                     .relation = "CONSUMES"});
    const auto& call_site = consume["call_site"];
    if (!require_fields(call_site, {"graph", "file", "line"}, "consumes.call_site", result)) {
      return result;
    }
    const auto graph_name = call_site["graph"].get<std::string>();
    const SeamGraph* graph = nullptr;
    if (!graph_for(graph_name, "consumes.call_site", &graph)) {
      return result;
    }
    const auto file = call_site["file"].get<std::string>();
    const auto line = call_site["line"].get<std::uint32_t>();
    const SeamNode* resolved = graph->resolve(file, line);
    if (resolved == nullptr) {
      return (void)fail(result, "anchor did not resolve to any node in graph '" + graph_name +
                                    "': " + file + ":" + std::to_string(line) +
                                    " (refusing to emit a dangling edge)"),
             result;
    }
    add_shadow(graph_name, *resolved);
    edges.push_back({.source = eid, .target = resolved->id, .relation = "CONSUMED_AT"});
    result.resolution_log.push_back("CONSUMED_AT  " + method + " " + path + "  ->  " + resolved->id);
  }

  // 5. MIRRORED_BY (schema->resolved consumer node)
  for (const auto& mirror : spec["mirrors"]) {
    if (!require_fields(mirror, {"schema", "graph", "file", "line"}, "mirrors", result)) {
      return result;
    }
    const auto schema_name = mirror["schema"].get<std::string>();
    const auto sid = schema_id(provider, api_version, schema_name);
    if (!has_node(sid)) {
      return (void)fail(result, "mirror references unknown schema: " + schema_name), result;
    }
    const auto graph_name = mirror["graph"].get<std::string>();
    const SeamGraph* graph = nullptr;
    if (!graph_for(graph_name, "mirrors", &graph)) {
      return result;
    }
    const auto file = mirror["file"].get<std::string>();
    const auto line = mirror["line"].get<std::uint32_t>();
    const SeamNode* resolved = graph->resolve(file, line);
    if (resolved == nullptr) {
      return (void)fail(result, "anchor did not resolve to any node in graph '" + graph_name +
                                    "': " + file + ":" + std::to_string(line) +
                                    " (refusing to emit a dangling edge)"),
             result;
    }
    add_shadow(graph_name, *resolved);
    edges.push_back({.source = sid, .target = resolved->id, .relation = "MIRRORED_BY"});
    result.resolution_log.push_back("MIRRORED_BY  " + schema_name + "  ->  " + resolved->id);
  }

  result.fragment.nodes = std::move(nodes);
  result.fragment.edges = std::move(edges);
  return result;
}

// Shadow code-ref for a node of `graph_name`, shared by generate_seam's anchors
// and discover_seam's handlers and call sites.
namespace {
Node code_ref_shadow(const std::string& graph_name, const SeamNode& node) {
  Node shadow;
  shadow.id = node.id;
  shadow.label = node.label;
  shadow.kind = "code-ref";
  shadow.source_file = node.source_file;
  shadow.properties["service"] = graph_name;
  shadow.properties["symbol_kind"] = node.kind;
  shadow.properties["span"] = std::to_string(node.start_line) + "-" + std::to_string(node.end_line);
  return shadow;
}
}  // namespace

SeamResult discover_seam(const std::vector<std::pair<std::string, std::filesystem::path>>& graphs) {
  SeamResult result;
  result.ok = true;
  if (graphs.empty()) {
    return (void)fail(result, "seam discover needs at least one --graph NAME=graph.json"), result;
  }

  std::vector<std::pair<std::string, SeamGraph>> loaded;
  for (const auto& [name, path] : graphs) {
    std::string error;
    auto graph = SeamGraph::load(path, error);
    if (!graph) {
      return (void)fail(result, "graph '" + name + "': " + error), result;
    }
    loaded.emplace_back(name, std::move(*graph));
  }

  // Insertion-ordered dedup, as in generate_seam: services, then each graph's
  // endpoints in graph order, then shadows, so regenerating is byte-stable.
  std::vector<Node> nodes;
  std::unordered_map<std::string, std::size_t> index;
  std::vector<Edge> edges;
  std::unordered_set<std::string> seen_edges;
  auto add_node = [&](Node node) {
    if (index.contains(node.id)) {
      return;
    }
    index.emplace(node.id, nodes.size());
    nodes.push_back(std::move(node));
  };
  auto add_edge = [&](std::string source, std::string target, std::string relation) {
    if (seen_edges.insert(source + "\x1f" + target + "\x1f" + relation).second) {
      edges.push_back({.source = std::move(source), .target = std::move(target), .relation = std::move(relation)});
    }
  };

  for (const auto& [name, graph] : loaded) {
    Node service;
    service.id = service_id(name);
    service.label = name;
    service.kind = "service";
    service.properties["role"] = "discovered";
    service.properties["graph"] = name;
    add_node(std::move(service));
  }

  std::unordered_map<std::string, std::unordered_set<std::string>> served_by;    // endpoint -> services
  std::unordered_map<std::string, std::unordered_set<std::string>> consumed_by;  // endpoint -> services
  for (const auto& [name, graph] : loaded) {
    std::unordered_map<std::string, std::vector<const SeamEdge*>> handled;   // endpoint -> handled_by edges
    std::unordered_map<std::string, std::vector<const SeamEdge*>> consumed;  // endpoint -> CONSUMES edges
    for (const auto& edge : graph.edges()) {
      if (edge.relation == "handled_by") {
        handled[edge.source].push_back(&edge);
      } else if (edge.relation == "CONSUMES") {
        consumed[edge.target].push_back(&edge);
      }
    }
    for (const auto& node : graph.nodes()) {
      if (node.kind != "endpoint") {
        continue;
      }
      const bool served = handled.contains(node.id);
      const bool used = consumed.contains(node.id);
      if (!served && !used) {
        continue;
      }
      Node endpoint;
      endpoint.id = node.id;
      endpoint.label = node.label;
      endpoint.kind = "endpoint";
      for (const auto* key : {"method", "path"}) {
        if (const auto value = node.properties.find(key); value != node.properties.end()) {
          endpoint.properties[key] = value->second;
        }
      }
      if (const auto existing = index.find(node.id); existing != index.end()) {
        // A served copy carries the provider's spelling and anchor; it wins
        // over a consumer's canonical placeholder copy.
        if (served && nodes[existing->second].properties.contains("served")) {
          nodes[existing->second].label = endpoint.label;
          nodes[existing->second].properties.erase("served");
          nodes[existing->second].properties["path"] = endpoint.properties["path"];
        }
      } else {
        if (!served) {
          endpoint.properties["served"] = "false";
        }
        add_node(std::move(endpoint));
      }
      if (served) {
        served_by[node.id].insert(name);
        add_edge(node.id, service_id(name), "SERVED_BY");
        for (const auto* edge : handled[node.id]) {
          if (const auto* handler = graph.find(edge->target)) {
            add_node(code_ref_shadow(name, *handler));
            add_edge(node.id, handler->id, "HANDLED_BY");
          }
        }
      }
      if (used) {
        consumed_by[node.id].insert(name);
        add_edge(service_id(name), node.id, "CONSUMES");
        for (const auto* edge : consumed[node.id]) {
          if (const auto* caller = graph.find(edge->source)) {
            add_node(code_ref_shadow(name, *caller));
            add_edge(node.id, caller->id, "CONSUMED_AT");
          }
        }
      }
    }
  }

  std::size_t matched = 0;
  std::size_t consumer_only = 0;
  std::size_t provider_only = 0;
  for (const auto& node : nodes) {
    if (node.kind != "endpoint") {
      continue;
    }
    const bool served = served_by.contains(node.id);
    const bool used = consumed_by.contains(node.id);
    if (served && used) {
      ++matched;
    } else if (used) {
      ++consumer_only;
    } else {
      ++provider_only;
    }
  }
  for (const auto& [name, graph] : loaded) {
    std::size_t serves = 0;
    std::size_t consumes = 0;
    for (const auto& [endpoint, services] : served_by) {
      serves += services.contains(name) ? 1 : 0;
    }
    for (const auto& [endpoint, services] : consumed_by) {
      consumes += services.contains(name) ? 1 : 0;
    }
    result.resolution_log.push_back("service " + name + ": serves " + std::to_string(serves) + " endpoints, consumes " +
                                    std::to_string(consumes));
  }
  result.resolution_log.push_back("matched " + std::to_string(matched) +
                                  " endpoints (served by one service, consumed by another or itself); " +
                                  std::to_string(consumer_only) + " consumed with no provider among these graphs; " +
                                  std::to_string(provider_only) + " served with no consumer");
  if (matched == 0 && consumer_only == 0 && provider_only == 0) {
    result.resolution_log.push_back("no endpoint nodes: build the graphs with a cgraph that discovers contracts");
  }
  result.fragment.nodes = std::move(nodes);
  result.fragment.edges = std::move(edges);
  return result;
}

}  // namespace cgraph
