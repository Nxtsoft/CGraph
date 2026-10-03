#include "cgraph/seam.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/endpoint_prefixes.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
                         const std::vector<std::pair<std::string, GraphSnapshot>>& services,
                         std::span<const EndpointPrefix> prefixes, std::span<const ContractDatabase> databases,
                         std::span<const EnvProvider> env) {
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

  // Node ids are project-relative, so two services can both own `src_db_client_ts`.
  // Scope every service-local id by its service; contract ids are shared on
  // purpose, since that is where a provider and its consumers meet: the ids
  // crossing_id gives, the spelling discover_seam joined them at. A
  // `table:local:` id or an undeclared env id is a service's own.
  auto scoped = [&](const std::string& service, const std::string& id) {
    if (auto shared = crossing_id(databases, env, service, id)) {
      return std::move(*shared);
    }
    return id.starts_with("service:") || id.starts_with("schema:") || service.empty() ? id : service + "::" + id;
  };

  // 1. Service code graphs: one community per service; real service nodes are authoritative.
  //    A call through the service's own proxy prefix lands on the proxied
  //    endpoint, as discover_seam joined it; the placeholder it leaves unused is
  //    not rendered.
  std::vector<std::string> unjoined;  // proxied endpoints no seam or service node carries
  std::unordered_set<std::string> endpoint_ids;  // every endpoint a proxied call may land on
  std::unordered_map<std::string, std::unordered_set<std::string>> servers;  // endpoint -> services with a handler
  if (!prefixes.empty()) {
    for (const auto& [name, graph] : services) {
      for (const auto& edge : graph.edges) {
        if (edge.relation == "handled_by") {
          servers[edge.source].insert(name);
        }
      }
    }
    for (const auto& node : seam.nodes) {
      if (node.kind == "endpoint") {
        endpoint_ids.insert(node.id);
      }
    }
    for (const auto& service : services) {
      for (const auto& node : service.second.nodes) {
        if (node.kind == "endpoint") {
          endpoint_ids.insert(node.id);
        }
      }
    }
  }
  for (const auto& [name, graph] : services) {
    std::unordered_map<std::string, std::string> proxied;  // placeholder id -> proxied id
    for (const auto& node : graph.nodes) {
      if (node.kind == "endpoint" && node.properties.contains("served") && !node.properties.contains("documented")) {
        auto target = proxied_endpoint_id(prefixes, name, node.id);
        // As in discover: never onto an endpoint only this service serves.
        if (const auto owners = target ? servers.find(*target) : servers.end();
            target && (owners == servers.end() || proxy_crosses_at(owners->second.size(), owners->second.contains(name)))) {
          proxied.emplace(node.id, std::move(*target));
        }
      }
    }
    for (const auto& node : graph.nodes) {
      if (proxied.contains(node.id)) {
        continue;
      }
      Node tagged = node;
      tagged.id = scoped(name, node.id);
      if (declared_contract_id(databases, name, node.id)) {
        tagged.properties["database"] = declared_database(databases, name)->name;  // was `local`
      }
      tagged.properties["community"] = name;
      tagged.properties.try_emplace("service", name);
      put(std::move(tagged), /*authoritative=*/true);
    }
    for (const auto& edge : graph.edges) {
      const auto target = proxied.find(edge.target);
      if (target != proxied.end()) {
        if (!endpoint_ids.contains(target->second)) {
          unjoined.push_back(name + ": " + edge.target + " -> " + target->second);
          continue;
        }
      }
      add_edge({.source = scoped(name, edge.source),
                .target = target != proxied.end() ? target->second : scoped(name, edge.target),
                .relation = edge.relation});
    }
  }
  if (!unjoined.empty()) {
    result.ok = false;
    result.errors.push_back(std::to_string(unjoined.size()) +
                            " proxied endpoint(s) are in neither the seam nor a service graph (discover the seam "
                            "with the same --prefix): " + unjoined.front());
    return result;
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
  // A seam edge into a service's code carries that service, so it lands on that
  // service's scoped node even when two services own the same raw id. An edge to
  // a code-ref with no service came from an older seam and cannot be placed.
  std::unordered_set<std::string> shadow_ids;
  for (const auto& node : seam.nodes) {
    if (node.kind == "code-ref") {
      shadow_ids.insert(node.id);
    }
  }
  std::vector<std::string> unplaced;
  for (const auto& edge : seam.edges) {
    const auto service = edge.properties.find("service");
    if (service == edge.properties.end()) {
      if (shadow_ids.contains(edge.source) || shadow_ids.contains(edge.target)) {
        unplaced.push_back(edge.relation + ": " + edge.source + " -> " + edge.target);
        continue;
      }
      add_edge(edge);
      continue;
    }
    add_edge({.source = scoped(service->second, edge.source), .target = scoped(service->second, edge.target),
              .relation = edge.relation});
  }
  // A contract discover joined under a declaration (`--env`, `--database`)
  // must be held under one of its spellings by every service the seam says
  // provides or uses it; fused without the same declarations, that service's
  // node would be scoped away from the seam's id and the join would silently
  // split, so it is refused, as an unjoined proxied endpoint is.
  std::unordered_map<std::string, std::unordered_set<std::string>> held;  // service -> its node ids
  for (const auto& [name, graph] : services) {
    auto& ids = held[name];
    for (const auto& node : graph.nodes) {
      ids.insert(node.id);
    }
  }
  std::vector<std::string> undeclared;
  for (const auto& edge : seam.edges) {
    const auto service = edge.properties.find("service");
    const auto kind = contract_kind_of(edge.source);
    if (service == edge.properties.end() || kind.empty() || kind == "endpoint" || !held.contains(service->second)) {
      continue;
    }
    const auto spellings = contract_spellings(databases, env, service->second, edge.source);
    const auto& ids = held.at(service->second);
    if (std::ranges::none_of(spellings, [&](const std::string& id) { return ids.contains(id); })) {
      undeclared.push_back("seam contract " + edge.source + " (" + edge.relation + " in " + service->second +
                           ") was joined by discover under a declaration fuse was not given; pass fuse the same " +
                           (kind == "env" ? "--env" : "--database") + " as discover");
    }
  }
  if (!undeclared.empty()) {
    result.ok = false;
    result.errors.push_back(std::to_string(undeclared.size()) + " seam contract edge(s) would split: " +
                            undeclared.front());
    return result;
  }
  if (!unplaced.empty()) {
    result.ok = false;
    result.errors.push_back(std::to_string(unplaced.size()) +
                            " seam edge(s) into service code name no service (regenerate the seam with this "
                            "version of `seam discover` or `seam generate`): " + unplaced.front());
    return result;
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
    edges.push_back({.source = eid, .target = resolved->id, .relation = "CONSUMED_AT", .properties = {{"service", graph_name}}});
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
    edges.push_back({.source = sid, .target = resolved->id, .relation = "MIRRORED_BY", .properties = {{"service", graph_name}}});
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

SeamResult discover_seam(const std::vector<std::pair<std::string, std::filesystem::path>>& graphs,
                         std::span<const EndpointPrefix> prefixes, std::span<const ContractDatabase> databases,
                         std::span<const EnvProvider> env) {
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
  // An edge into a service's code carries that service: two services can own the
  // same project-relative id, so the id alone does not say whose code it is.
  auto add_edge = [&](std::string source, std::string target, std::string relation, const std::string& service = {},
                      const std::string& via = {}) {
    if (seen_edges.insert(source + "\x1f" + target + "\x1f" + relation + "\x1f" + service).second) {
      Edge edge{.source = std::move(source), .target = std::move(target), .relation = std::move(relation)};
      if (!service.empty()) {
        edge.properties["service"] = service;
      }
      if (!via.empty()) {
        edge.properties["via"] = via;
      }
      edges.push_back(std::move(edge));
    }
  };
  // Consumed endpoints each prefix mapped, in prefix order, for the log.
  std::vector<std::size_t> mapped_per_prefix(prefixes.size(), 0);
  // Repo-local tables and labels each declared database joined, for the log.
  std::vector<std::size_t> mapped_per_database(databases.size(), 0);

  for (const auto& [name, graph] : loaded) {
    Node service;
    service.id = service_id(name);
    service.label = name;
    service.kind = "service";
    service.properties["role"] = "discovered";
    service.properties["graph"] = name;
    add_node(std::move(service));
  }

  // Who serves what, before any join: a proxied call forwards to another
  // service, so it never joins an endpoint only its own service serves.
  std::unordered_map<std::string, std::unordered_set<std::string>> servers;  // endpoint -> services with a handler
  if (!prefixes.empty()) {
    for (const auto& [name, graph] : loaded) {
      for (const auto& edge : graph.edges()) {
        if (edge.relation == "handled_by") {
          servers[edge.source].insert(name);
        }
      }
    }
  }
  std::unordered_map<std::string, std::unordered_set<std::string>> served_by;      // endpoint -> services
  std::unordered_map<std::string, std::unordered_set<std::string>> consumed_by;    // endpoint -> services
  std::unordered_map<std::string, std::unordered_set<std::string>> documented_by;  // endpoint -> services
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
    // The document node an endpoint was declared in, by source path.
    std::unordered_map<std::string, const SeamNode*> file_by_path;
    for (const auto& node : graph.nodes()) {
      if (node.kind == "file") {
        file_by_path.emplace(node.source_file, &node);
      }
    }
    for (const auto& node : graph.nodes()) {
      // Every contract that crosses repositories joins here (crossing_id): a
      // repo-local table or label only under a database its repo declares, an
      // env variable only when declared.
      const auto shared = crossing_id(databases, env, name, node.id);
      if (!shared) {
        continue;
      }
      const bool served = handled.contains(node.id);
      const bool used = consumed.contains(node.id);
      const bool documented = node.properties.contains("documented");
      if (!served && !used && !documented) {
        continue;
      }
      Node endpoint;
      endpoint.id = *shared;
      endpoint.label = node.label;
      endpoint.kind = node.kind;
      for (const auto* key : {"method", "path", "name", "database"}) {
        if (const auto value = node.properties.find(key); value != node.properties.end()) {
          endpoint.properties[key] = value->second;
        }
      }
      if (*shared != node.id) {  // a member's repo-local table, joined at its database's id
        const auto* database = declared_database(databases, name);
        endpoint.properties["database"] = database->name;
        ++mapped_per_database[static_cast<std::size_t>(database - databases.data())];
      }
      // A call this service only consumes, through its own proxy prefix, joins
      // at the path the proxy forwards to. Its own spelling stays on the edge.
      std::string via;
      for (std::size_t slot = 0; !served && !documented && slot < prefixes.size(); ++slot) {
        auto proxied = proxied_endpoint_id(prefixes.subspan(slot, 1), name, node.id);
        if (!proxied) {
          continue;
        }
        if (const auto owners = servers.find(*proxied);
            owners != servers.end() && !proxy_crosses_at(owners->second.size(), owners->second.contains(name))) {
          break;  // only this service serves the proxied path: not where the proxy forwards to
        }
        ++mapped_per_prefix[slot];
        via = endpoint.properties["path"];
        // `endpoint:<METHOD> <path>`: the proxied id is canonical, so it is its own spelling.
        const auto space = proxied->find(' ');
        endpoint.properties["method"] = proxied->substr(9, space - 9);
        endpoint.properties["path"] = proxied->substr(space + 1);
        endpoint.label = proxied->substr(9);
        endpoint.id = std::move(*proxied);
        break;
      }
      const std::string eid = endpoint.id;
      if (const auto existing = index.find(eid); existing != index.end()) {
        // A served or documented copy carries the provider's spelling; it wins
        // over a consumer's canonical placeholder copy.
        if ((served || documented) && nodes[existing->second].properties.contains("served")) {
          nodes[existing->second].label = endpoint.label;
          nodes[existing->second].properties.erase("served");
          if (const auto path = endpoint.properties.find("path"); path != endpoint.properties.end()) {
            nodes[existing->second].properties["path"] = path->second;
          }
        }
      } else {
        if (!served && !documented) {
          endpoint.properties["served"] = "false";
        }
        add_node(std::move(endpoint));
      }
      if (documented) {
        documented_by[eid].insert(name);
        if (const auto file = file_by_path.find(node.source_file); file != file_by_path.end()) {
          add_node(code_ref_shadow(name, *file->second));
          add_edge(eid, file->second->id, "DOCUMENTED_IN", name);
        }
      }
      if (served) {
        served_by[eid].insert(name);
        add_edge(eid, service_id(name), "SERVED_BY");
        for (const auto* edge : handled[node.id]) {
          if (const auto* handler = graph.find(edge->target)) {
            add_node(code_ref_shadow(name, *handler));
            add_edge(eid, handler->id, "HANDLED_BY", name);
          }
        }
      }
      if (used) {
        consumed_by[eid].insert(name);
        add_edge(service_id(name), eid, "CONSUMES");
        for (const auto* edge : consumed[node.id]) {
          if (const auto* caller = graph.find(edge->source)) {
            add_node(code_ref_shadow(name, *caller));
            add_edge(eid, caller->id, "CONSUMED_AT", name, via);
          }
        }
      }
    }
  }

  // An env variable is provided by the service the declarations say it
  // addresses, which holds no node of it.
  for (auto& node : nodes) {
    const auto provider = env_provider_of(env, node.id);
    if (!provider || !std::ranges::any_of(loaded, [&](const auto& graph) { return graph.first == *provider; })) {
      continue;
    }
    node.properties.erase("served");
    served_by[node.id].insert(*provider);
    add_edge(node.id, service_id(*provider), "SERVED_BY");
  }

  // Contracts other than endpoints, counted on their own line so the
  // endpoint lines read as they always have.
  std::size_t other_matched = 0;
  std::size_t other_used_only = 0;
  std::size_t other_provided_only = 0;
  for (const auto& node : nodes) {
    if (node.kind == "endpoint" || node.kind == "service" || node.kind == "code-ref") {
      continue;
    }
    const bool provided = served_by.contains(node.id);
    const bool used = consumed_by.contains(node.id);
    other_matched += provided && used ? 1 : 0;
    other_used_only += used && !provided ? 1 : 0;
    other_provided_only += provided && !used ? 1 : 0;
  }

  std::size_t matched = 0;
  std::size_t consumer_only = 0;
  std::size_t provider_only = 0;
  std::size_t documented_only = 0;
  std::size_t documented_not_served = 0;
  std::size_t served_not_documented = 0;
  for (const auto& node : nodes) {
    if (node.kind != "endpoint") {
      continue;
    }
    const bool served = served_by.contains(node.id);
    const bool used = consumed_by.contains(node.id);
    const bool documented = documented_by.contains(node.id);
    if (served && used) {
      ++matched;
    } else if (used) {
      ++consumer_only;
    } else if (served) {
      ++provider_only;
    } else {
      ++documented_only;
    }
    if (documented && !served) {
      ++documented_not_served;
    }
    if (served && !documented) {
      ++served_not_documented;
    }
  }
  const auto is_endpoint = [](const std::string& id) { return contract_kind_of(id) == "endpoint"; };
  for (const auto& [name, graph] : loaded) {
    std::size_t serves = 0;
    std::size_t consumes = 0;
    std::size_t documents = 0;
    for (const auto& [endpoint, services] : served_by) {
      serves += is_endpoint(endpoint) && services.contains(name) ? 1 : 0;
    }
    for (const auto& [endpoint, services] : consumed_by) {
      consumes += is_endpoint(endpoint) && services.contains(name) ? 1 : 0;
    }
    for (const auto& [endpoint, services] : documented_by) {
      documents += services.contains(name) ? 1 : 0;
    }
    result.resolution_log.push_back("service " + name + ": serves " + std::to_string(serves) + " endpoints, consumes " +
                                    std::to_string(consumes) + ", documents " + std::to_string(documents));
  }
  result.resolution_log.push_back("matched " + std::to_string(matched) +
                                  " endpoints (served by one service, consumed by another or itself); " +
                                  std::to_string(consumer_only) + " consumed with no provider among these graphs; " +
                                  std::to_string(provider_only) + " served with no consumer");
  for (std::size_t slot = 0; slot < prefixes.size(); ++slot) {
    result.resolution_log.push_back("prefix " + prefixes[slot].repo + " " + prefixes[slot].from + " -> " +
                                    prefixes[slot].to + ": " + std::to_string(mapped_per_prefix[slot]) +
                                    " consumed endpoints joined at the proxied path");
  }
  if (other_matched + other_used_only + other_provided_only > 0) {
    result.resolution_log.push_back("other contracts (tables, graph labels, headers, claims, env, DynamoDB tables): matched " +
                                    std::to_string(other_matched) + "; " + std::to_string(other_used_only) +
                                    " used with no provider among these graphs; " +
                                    std::to_string(other_provided_only) + " provided with no user");
  }
  for (std::size_t slot = 0; slot < databases.size(); ++slot) {
    std::string members;
    for (const auto& repo : databases[slot].repos) {
      members += (members.empty() ? "" : ",") + repo;
    }
    result.resolution_log.push_back("database " + databases[slot].name + " (" + members + "): " +
                                    std::to_string(mapped_per_database[slot]) +
                                    " repo-local tables and labels joined at the database's id");
  }
  if (!documented_by.empty()) {
    // Contract drift: what the documents say against what the code serves.
    result.resolution_log.push_back("drift: " + std::to_string(documented_not_served) +
                                    " documented but served by no service here, " +
                                    std::to_string(served_not_documented) + " served but in no document; " +
                                    std::to_string(documented_only) + " only documented (neither served nor consumed)");
  }
  if (matched == 0 && consumer_only == 0 && provider_only == 0 && documented_only == 0 &&
      other_matched + other_used_only + other_provided_only == 0) {
    result.resolution_log.push_back("no endpoint nodes: build the graphs with a cgraph that discovers contracts");
  }
  result.fragment.nodes = std::move(nodes);
  result.fragment.edges = std::move(edges);
  return result;
}

}  // namespace cgraph
