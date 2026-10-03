#include "cgraph/seam.hpp"

#include "cgraph/contracts.hpp"
#include "cgraph/daemon_ops.hpp"
#include "cgraph/export_json.hpp"
#include "cgraph/fragment_json.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_fragment_validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

void write_json(const fs::path& path, const json& value) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << value.dump(2);
}

const cgraph::Node* find_node(const cgraph::Fragment& frag, const std::string& id) {
  for (const auto& node : frag.nodes) {
    if (node.id == id) {
      return &node;
    }
  }
  return nullptr;
}

bool has_edge(const cgraph::Fragment& frag, const std::string& src, const std::string& tgt,
              const std::string& rel) {
  for (const auto& edge : frag.edges) {
    if (edge.source == src && edge.target == tgt && edge.relation == rel) {
      return true;
    }
  }
  return false;
}

const cgraph::Node* find_in(const cgraph::GraphSnapshot& graph, const std::string& id) {
  for (const auto& node : graph.nodes) {
    if (node.id == id) {
      return &node;
    }
  }
  return nullptr;
}

bool has_snapshot_edge(const cgraph::GraphSnapshot& graph, const std::string& src,
                       const std::string& tgt, const std::string& rel) {
  for (const auto& edge : graph.edges) {
    if (edge.source == src && edge.target == tgt && edge.relation == rel) {
      return true;
    }
  }
  return false;
}

// The canonical spec used by the happy-path and (mutated) error-path tests.
json make_spec() {
  return json{
      {"provider", "ml-api"},
      {"api_version", "v3"},
      {"error_codes", {400, 500}},
      {"services",
       {{{"name", "ml-api"}, {"role", "provider"}, {"owned", false}},
        {{"name", "backend"}, {"role", "consumer"}, {"owned", true}}}},
      {"schemas", {{{"name", "ScoreResult"}, {"canonical", "ml-api/src/schemas/score.ts"}}}},
      {"endpoints",
       {{{"method", "POST"},
         {"path", "/v3/score"},
         {"response_schema", "ScoreResult"},
         {"path_params", {"modelId"}},
         {"query_params", {"verbose"}}}}},
      {"consumes",
       {{{"service", "backend"},
         {"method", "POST"},
         {"path", "/v3/score"},
         {"call_site", {{"graph", "backend"}, {"file", "src/score.ts"}, {"line", 42}}}}}},
      {"mirrors",
       {{{"schema", "ScoreResult"},
         {"graph", "backend"},
         {"file", "src/types.ts"},
         {"line", 10}}}}};
}

}  // namespace

// seam discover: the contracts each graph already carries become the fragment,
// with no spec. The api graph serves an endpoint (handled_by); the web graph
// consumes the same canonical id (CONSUMES) and one nobody serves.
int test_discover(const fs::path& root) {
  const auto api_graph = root / "api.json";
  const auto web_graph = root / "web.json";
  write_json(api_graph,
             json{{"nodes",
                   {{{"id", "api::handler"},
                     {"label", "notebookRoutes.get /starred-notes"},
                     {"type", "function"},
                     {"source_file", "api/src/modules/notebooks/index.ts"},
                     {"source_location", {{"start_line", 75}, {"end_line", 82}}}},
                    {{"id", "endpoint:GET /api/v1/notebooks/starred-notes"},
                     {"label", "GET /api/v1/notebooks/starred-notes"},
                     {"type", "endpoint"},
                     {"source_file", "api/src/modules/notebooks/index.ts"},
                     {"properties", {{"method", "GET"}, {"path", "/api/v1/notebooks/starred-notes"}}}},
                    {{"id", "endpoint:GET /api/v1/health"},
                     {"label", "GET /api/v1/health"},
                     {"type", "endpoint"},
                     {"properties", {{"method", "GET"}, {"path", "/api/v1/health"}}}}}},
                  {"links",
                   {{{"source", "endpoint:GET /api/v1/notebooks/starred-notes"}, {"target", "api::handler"}, {"relation", "handled_by"}},
                    {{"source", "endpoint:GET /api/v1/health"}, {"target", "api::missing-handler"}, {"relation", "handled_by"}}}}});
  write_json(web_graph,
             json{{"nodes",
                   {{{"id", "web::useStarred"},
                     {"label", "useStarred"},
                     {"type", "function"},
                     {"source_file", "web/lib/hooks/use-starred.ts"},
                     {"source_location", {{"start_line", 9}, {"end_line", 14}}}},
                    {{"id", "endpoint:GET /api/v1/notebooks/starred-notes"},
                     {"label", "GET /api/v1/notebooks/starred-notes"},
                     {"type", "endpoint"},
                     {"properties", {{"method", "GET"}, {"path", "/api/v1/notebooks/starred-notes"}, {"served", "false"}}}},
                    {{"id", "endpoint:POST /api/v1/orphan"},
                     {"label", "POST /api/v1/orphan"},
                     {"type", "endpoint"},
                     {"properties", {{"method", "POST"}, {"path", "/api/v1/orphan"}, {"served", "false"}}}},
                    {{"id", "endpoint:GET /api/v1/unused"},
                     {"label", "GET /api/v1/unused"},
                     {"type", "endpoint"},
                     {"properties", {{"method", "GET"}, {"path", "/api/v1/unused"}, {"served", "false"}}}}}},
                  {"links",
                   {{{"source", "web::useStarred"}, {"target", "endpoint:GET /api/v1/notebooks/starred-notes"}, {"relation", "CONSUMES"}},
                    {{"source", "web::useStarred"}, {"target", "endpoint:POST /api/v1/orphan"}, {"relation", "CONSUMES"}}}}});

  // A third graph holds only a contract document: its endpoints are documented,
  // neither served nor consumed there.
  const auto spec_graph = root / "spec.json";
  write_json(spec_graph,
             json{{"nodes",
                   {{{"id", "spec::file"}, {"label", "api/openapi.json"}, {"type", "file"}, {"source_file", "spec/api/openapi.json"}},
                    {{"id", "endpoint:GET /api/v1/notebooks/starred-notes"},
                     {"label", "GET /api/v1/notebooks/starred-notes"},
                     {"type", "endpoint"},
                     {"source_file", "spec/api/openapi.json"},
                     {"properties", {{"method", "GET"}, {"path", "/api/v1/notebooks/starred-notes"}, {"documented", "true"}}}},
                    {{"id", "endpoint:GET /api/v1/removed"},
                     {"label", "GET /api/v1/removed"},
                     {"type", "endpoint"},
                     {"source_file", "spec/api/openapi.json"},
                     {"properties", {{"method", "GET"}, {"path", "/api/v1/removed"}, {"documented", "true"}}}}}},
                  {"links", json::array()}});

  const auto res = cgraph::discover_seam({{"api", api_graph}, {"web", web_graph}, {"spec", spec_graph}});
  if (!res.ok || !res.errors.empty()) {
    return 1;
  }
  // Documented endpoints: DOCUMENTED_IN the document's shadow, kept when only
  // documented, and the drift line counts what the document and the code disagree on.
  if (!has_edge(res.fragment, "endpoint:GET /api/v1/notebooks/starred-notes", "spec::file", "DOCUMENTED_IN") ||
      find_node(res.fragment, "spec::file") == nullptr || find_node(res.fragment, "spec::file")->kind != "code-ref") {
    return 1;
  }
  const auto* removed = find_node(res.fragment, "endpoint:GET /api/v1/removed");
  if (removed == nullptr || removed->properties.contains("served")) {
    return 1;
  }
  bool drift_line = false;
  bool documents_line = false;
  for (const auto& line : res.resolution_log) {
    // starred-notes and removed are documented; only starred-notes is served -> 1
    // documented-but-unserved; api serves starred-notes and health, neither in the
    // document... starred-notes is: so 1 served-but-undocumented (health).
    drift_line = drift_line || line == "drift: 1 documented but served by no service here, 1 served but in no document; 1 only documented (neither served nor consumed)";
    documents_line = documents_line || line == "service spec: serves 0 endpoints, consumes 0, documents 2";
  }
  if (!drift_line || !documents_line) {
    for (const auto& line : res.resolution_log) std::cerr << "log: " << line << '\n';
    return 1;
  }
  const auto& frag = res.fragment;
  const std::string starred = "endpoint:GET /api/v1/notebooks/starred-notes";
  if (find_node(frag, "service:api") == nullptr || find_node(frag, "service:web") == nullptr) {
    return 1;
  }
  const auto* endpoint = find_node(frag, starred);
  if (endpoint == nullptr || endpoint->kind != "endpoint" || endpoint->properties.contains("served")) {
    return 1;  // the served copy wins; no `served: false` placeholder survives
  }
  if (!has_edge(frag, starred, "service:api", "SERVED_BY") || !has_edge(frag, "service:web", starred, "CONSUMES") ||
      !has_edge(frag, starred, "api::handler", "HANDLED_BY") || !has_edge(frag, starred, "web::useStarred", "CONSUMED_AT")) {
    return 1;
  }
  const auto* handler = find_node(frag, "api::handler");
  const auto* caller = find_node(frag, "web::useStarred");
  if (handler == nullptr || handler->kind != "code-ref" || handler->properties.at("service") != "api" ||
      caller == nullptr || caller->kind != "code-ref" || caller->properties.at("service") != "web") {
    return 1;
  }
  // A consumed endpoint no graph serves stays, marked; an endpoint neither served
  // nor consumed is not a contract; a handled_by whose handler node is missing
  // still marks the endpoint served but adds no shadow.
  const auto* orphan = find_node(frag, "endpoint:POST /api/v1/orphan");
  if (orphan == nullptr || orphan->properties.at("served") != "false" || find_node(frag, "endpoint:GET /api/v1/unused") != nullptr) {
    return 1;
  }
  if (!has_edge(frag, "endpoint:GET /api/v1/health", "service:api", "SERVED_BY") || find_node(frag, "api::missing-handler") != nullptr) {
    return 1;
  }
  bool matched_line = false;
  for (const auto& line : res.resolution_log) {
    matched_line = matched_line || line.find("matched 1 endpoints") != std::string::npos;
  }
  if (!matched_line) {
    return 1;
  }
  // The fragment ingests unchanged and fuses with the two service graphs.
  if (!cgraph::validate_semantic_fragment_json(cgraph::to_json(frag)).valid) {
    return 1;
  }
  cgraph::GraphSnapshot api_snapshot;
  api_snapshot.nodes.push_back({.id = "api::handler", .label = "handler", .source_file = "api/src/modules/notebooks/index.ts", .kind = "function"});
  api_snapshot.nodes.push_back({.id = starred, .label = "GET /api/v1/notebooks/starred-notes", .kind = "endpoint"});
  api_snapshot.nodes.push_back({.id = "endpoint:GET /api/v1/health", .label = "GET /api/v1/health", .kind = "endpoint"});
  api_snapshot.nodes.push_back({.id = "api::missing-handler", .label = "h", .kind = "function"});
  cgraph::GraphSnapshot web_snapshot;
  web_snapshot.nodes.push_back({.id = "web::useStarred", .label = "useStarred", .source_file = "web/lib/hooks/use-starred.ts", .kind = "function"});
  web_snapshot.nodes.push_back({.id = starred, .label = "GET /api/v1/notebooks/starred-notes", .kind = "endpoint"});
  web_snapshot.nodes.push_back({.id = "endpoint:POST /api/v1/orphan", .label = "POST /api/v1/orphan", .kind = "endpoint"});
  cgraph::GraphSnapshot spec_snapshot;
  spec_snapshot.nodes.push_back({.id = "spec::file", .label = "api/openapi.json", .source_file = "spec/api/openapi.json", .kind = "file"});
  spec_snapshot.nodes.push_back({.id = starred, .label = "GET /api/v1/notebooks/starred-notes", .kind = "endpoint"});
  spec_snapshot.nodes.push_back({.id = "endpoint:GET /api/v1/removed", .label = "GET /api/v1/removed", .kind = "endpoint"});
  const auto fused = cgraph::fuse_seam(frag, {{"api", api_snapshot}, {"web", web_snapshot}, {"spec", spec_snapshot}});
  if (!fused.ok || find_in(fused.graph, starred) == nullptr ||
      !has_snapshot_edge(fused.graph, "service:web", starred, "CONSUMES")) {
    return 1;
  }
  // Byte-stable regeneration.
  const auto again = cgraph::discover_seam({{"api", api_graph}, {"web", web_graph}, {"spec", spec_graph}});
  if (!again.ok || cgraph::to_json(again.fragment).dump() != cgraph::to_json(frag).dump()) {
    return 1;
  }
  // A missing graph is a hard error.
  if (cgraph::discover_seam({{"api", root / "nope.json"}}).ok) {
    return 1;
  }
  return 0;
}

// Two services with the same project-relative file must stay two nodes in the
// fused graph: ids are scoped by service, while contract ids stay shared so a
// provider and its consumer still meet.
int test_fuse_same_relative_file() {
  auto repo = [](const std::string& file, const std::string& importer) {
    cgraph::GraphSnapshot graph;
    graph.nodes.push_back(cgraph::Node{.id = "src_db_client_ts", .label = "db/client.ts", .source_file = file,
                                       .kind = "file"});
    graph.nodes.push_back(cgraph::Node{.id = importer, .label = importer, .kind = "file"});
    graph.nodes.push_back(cgraph::Node{.id = "endpoint:GET /x", .label = "GET /x", .kind = "endpoint"});
    graph.edges.push_back(cgraph::Edge{.source = importer, .target = "src_db_client_ts", .relation = "imports_from"});
    graph.edges.push_back(cgraph::Edge{.source = importer, .target = "endpoint:GET /x", .relation = "CONSUMES"});
    return graph;
  };
  const cgraph::Fragment no_seam;
  const auto fused = cgraph::fuse_seam(no_seam, {{"api", repo("api/src/db/client.ts", "src_a_ts")},
                                                 {"agents", repo("agents/src/db/client.ts", "src_b_ts")}});
  const auto* api_client = find_in(fused.graph, "api::src_db_client_ts");
  const auto* agents_client = find_in(fused.graph, "agents::src_db_client_ts");
  if (!fused.ok || api_client == nullptr || agents_client == nullptr ||
      api_client->source_file != "api/src/db/client.ts" || agents_client->source_file != "agents/src/db/client.ts" ||
      find_in(fused.graph, "src_db_client_ts") != nullptr) {
    std::cerr << "fuse: same relative file did not stay two scoped nodes\n";
    return 1;
  }
  if (!has_snapshot_edge(fused.graph, "api::src_a_ts", "api::src_db_client_ts", "imports_from") ||
      has_snapshot_edge(fused.graph, "api::src_a_ts", "agents::src_db_client_ts", "imports_from") ||
      !has_snapshot_edge(fused.graph, "agents::src_b_ts", "agents::src_db_client_ts", "imports_from")) {
    std::cerr << "fuse: an import edge crossed services\n";
    return 1;
  }
  if (find_in(fused.graph, "endpoint:GET /x") == nullptr ||
      !has_snapshot_edge(fused.graph, "api::src_a_ts", "endpoint:GET /x", "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "agents::src_b_ts", "endpoint:GET /x", "CONSUMES")) {
    std::cerr << "fuse: the shared endpoint was not shared\n";
    return 1;
  }
  return 0;
}

// Two services whose callers share a raw id (`src_api_ts`) both consume one
// endpoint. Discover keeps both CONSUMED_AT edges, each stamped with its
// service, and fuse lands each on its own service's node. An unstamped edge
// into service code (an older seam) is refused, never guessed.
int test_shared_raw_id(const fs::path& root) {
  const auto endpoint = std::string("endpoint:GET /api/v1/x");
  auto consumer_graph = [&](const std::string& file) {
    return json{{"nodes",
                 {{{"id", "src_api_ts"}, {"label", "src/api.ts"}, {"type", "file"}, {"source_file", file},
                   {"source_location", {{"start_line", 1}, {"end_line", 9}}}},
                  {{"id", endpoint}, {"label", "GET /api/v1/x"}, {"type", "endpoint"},
                   {"properties", {{"method", "GET"}, {"path", "/api/v1/x"}, {"served", "false"}}}}}},
                {"links", {{{"source", "src_api_ts"}, {"target", endpoint}, {"relation", "CONSUMES"}}}}};
  };
  const auto web_graph = root / "shared-web.json";
  const auto worker_graph = root / "shared-worker.json";
  write_json(web_graph, consumer_graph("web/src/api.ts"));
  write_json(worker_graph, consumer_graph("worker/src/api.ts"));
  const auto res = cgraph::discover_seam({{"web", web_graph}, {"worker", worker_graph}});
  int stamped = 0;
  for (const auto& edge : res.fragment.edges) {
    if (edge.relation == "CONSUMED_AT" && edge.source == endpoint && edge.target == "src_api_ts") {
      const auto service = edge.properties.find("service");
      stamped += service != edge.properties.end() && (service->second == "web" || service->second == "worker") ? 1 : 0;
    }
  }
  if (!res.ok || stamped != 2) {
    std::cerr << "discover: expected two service-stamped CONSUMED_AT edges, got " << stamped << "\n";
    return 1;
  }
  auto snapshot = [&](const fs::path& path) {
    std::ifstream input(path);
    json graph;
    input >> graph;
    return cgraph::parse_node_link_graph(graph);
  };
  const auto fused = cgraph::fuse_seam(res.fragment, {{"web", snapshot(web_graph)}, {"worker", snapshot(worker_graph)}});
  if (!fused.ok || !has_snapshot_edge(fused.graph, endpoint, "web::src_api_ts", "CONSUMED_AT") ||
      !has_snapshot_edge(fused.graph, endpoint, "worker::src_api_ts", "CONSUMED_AT")) {
    std::cerr << "fuse: a shared raw id did not land on each service's own node\n";
    return 1;
  }
  cgraph::Fragment old_seam = res.fragment;
  for (auto& edge : old_seam.edges) {
    edge.properties.erase("service");
  }
  const auto refused = cgraph::fuse_seam(old_seam, {{"web", snapshot(web_graph)}, {"worker", snapshot(worker_graph)}});
  if (refused.ok || refused.errors.empty() || !refused.graph.nodes.empty()) {
    std::cerr << "fuse: an unstamped edge into service code was placed instead of refused\n";
    return 1;
  }
  return 0;
}

// A front end reaching its backend through its own catch-all proxy
// (`/api/backend/*` forwarded to `/api/*`): with a prefix, discover joins the
// consumer's `/api/backend/v1/sessions/{}/extend` to the backend's
// `/api/v1/sessions/{}/extend`, keeps the consumer's spelling on the edge, and
// leaves a route the front end serves itself (`/api/backend/healthz`) alone.
// Fuse redirects the service's CONSUMES edge to the joined endpoint.
int test_proxy_prefix(const fs::path& root) {
  const std::string proxied = "endpoint:PATCH /api/backend/v1/sessions/{}/extend";
  const std::string provided = "endpoint:PATCH /api/v1/sessions/{}/extend";
  const std::string local = "endpoint:GET /api/backend/healthz";
  const std::string backend_health = "endpoint:GET /api/healthz";
  // The front end also serves /api/saml/metadata itself; the proxy never
  // forwards to its own routes, so /api/backend/saml/metadata stays unjoined.
  const std::string own_route = "endpoint:GET /api/saml/metadata";
  const std::string proxied_own = "endpoint:GET /api/backend/saml/metadata";
  const auto web_graph = root / "proxy-web.json";
  const auto idp_graph = root / "proxy-idp.json";
  write_json(web_graph,
             json{{"nodes",
                   {{{"id", "hooks_sessions_ts_extend"}, {"label", "extendSession"}, {"type", "function"},
                     {"source_file", "web/hooks/sessions.ts"}, {"source_location", {{"start_line", 3}, {"end_line", 9}}}},
                    {{"id", "app_api_backend_healthz_route_ts_get"}, {"label", "GET"}, {"type", "function"},
                     {"source_file", "web/app/api/backend/healthz/route.ts"},
                     {"source_location", {{"start_line", 1}, {"end_line", 4}}}},
                    {{"id", proxied}, {"label", "PATCH /api/backend/v1/sessions/{}/extend"}, {"type", "endpoint"},
                     {"properties", {{"method", "PATCH"}, {"path", "/api/backend/v1/sessions/{}/extend"}, {"served", "false"}}}},
                    {{"id", "app_api_saml_metadata_route_ts_get"}, {"label", "GET"}, {"type", "function"},
                     {"source_file", "web/app/api/saml/metadata/route.ts"},
                     {"source_location", {{"start_line", 1}, {"end_line", 4}}}},
                    {{"id", own_route}, {"label", "GET /api/saml/metadata"}, {"type", "endpoint"},
                     {"source_file", "web/app/api/saml/metadata/route.ts"},
                     {"properties", {{"method", "GET"}, {"path", "/api/saml/metadata"}}}},
                    {{"id", proxied_own}, {"label", "GET /api/backend/saml/metadata"}, {"type", "endpoint"},
                     {"properties", {{"method", "GET"}, {"path", "/api/backend/saml/metadata"}, {"served", "false"}}}},
                    {{"id", local}, {"label", "GET /api/backend/healthz"}, {"type", "endpoint"},
                     {"source_file", "web/app/api/backend/healthz/route.ts"},
                     {"properties", {{"method", "GET"}, {"path", "/api/backend/healthz"}}}}}},
                  {"links",
                   {{{"source", "hooks_sessions_ts_extend"}, {"target", proxied}, {"relation", "CONSUMES"}},
                    {{"source", "hooks_sessions_ts_extend"}, {"target", local}, {"relation", "CONSUMES"}},
                    {{"source", local}, {"target", "app_api_backend_healthz_route_ts_get"}, {"relation", "handled_by"}},
                    {{"source", "hooks_sessions_ts_extend"}, {"target", proxied_own}, {"relation", "CONSUMES"}},
                    {{"source", own_route}, {"target", "app_api_saml_metadata_route_ts_get"}, {"relation", "handled_by"}}}}});
  write_json(idp_graph,
             json{{"nodes",
                   {{{"id", "sessioncontroller_extend"}, {"label", "extendSession"}, {"type", "method"},
                     {"source_file", "idp/SessionController.kt"}, {"source_location", {{"start_line", 280}, {"end_line", 290}}}},
                    {{"id", "healthcontroller_health"}, {"label", "health"}, {"type", "method"},
                     {"source_file", "idp/HealthController.kt"}, {"source_location", {{"start_line", 5}, {"end_line", 7}}}},
                    {{"id", provided}, {"label", "PATCH /api/v1/sessions/{id}/extend"}, {"type", "endpoint"},
                     {"source_file", "idp/SessionController.kt"},
                     {"properties", {{"method", "PATCH"}, {"path", "/api/v1/sessions/{id}/extend"}}}},
                    {{"id", backend_health}, {"label", "GET /api/healthz"}, {"type", "endpoint"},
                     {"source_file", "idp/HealthController.kt"}, {"properties", {{"method", "GET"}, {"path", "/api/healthz"}}}}}},
                  {"links",
                   {{{"source", provided}, {"target", "sessioncontroller_extend"}, {"relation", "handled_by"}},
                    {{"source", backend_health}, {"target", "healthcontroller_health"}, {"relation", "handled_by"}}}}});
  const std::vector<cgraph::EndpointPrefix> prefixes{{.repo = "web", .from = "/api/backend", .to = "/api"}};

  // Without the prefix the two spellings never meet.
  const auto plain = cgraph::discover_seam({{"idp", idp_graph}, {"web", web_graph}});
  if (!plain.ok || has_edge(plain.fragment, provided, "hooks_sessions_ts_extend", "CONSUMED_AT")) {
    std::cerr << "discover: the proxied call met its provider without a prefix\n";
    return 1;
  }

  const auto res = cgraph::discover_seam({{"idp", idp_graph}, {"web", web_graph}}, prefixes);
  if (!res.ok || !has_edge(res.fragment, provided, "hooks_sessions_ts_extend", "CONSUMED_AT") ||
      !has_edge(res.fragment, "service:web", provided, "CONSUMES") ||
      !has_edge(res.fragment, provided, "sessioncontroller_extend", "HANDLED_BY")) {
    std::cerr << "discover: the proxied call did not join the provider's endpoint\n";
    return 1;
  }
  if (find_node(res.fragment, proxied) != nullptr) {
    std::cerr << "discover: the consumer's proxy spelling stayed a separate endpoint\n";
    return 1;
  }
  const auto* joined = find_node(res.fragment, provided);
  if (joined == nullptr || joined->properties.contains("served") || joined->label != "PATCH /api/v1/sessions/{id}/extend") {
    std::cerr << "discover: the joined endpoint should carry the provider's spelling\n";
    return 1;
  }
  bool via = false;
  for (const auto& edge : res.fragment.edges) {
    if (edge.relation == "CONSUMED_AT" && edge.source == provided && edge.target == "hooks_sessions_ts_extend") {
      const auto found = edge.properties.find("via");
      via = found != edge.properties.end() && found->second == "/api/backend/v1/sessions/{}/extend";
    }
  }
  if (!via) {
    std::cerr << "discover: CONSUMED_AT should keep the consumer's own path as `via`\n";
    return 1;
  }
  // The front end serves /api/backend/healthz itself: its call stays local.
  if (!has_edge(res.fragment, local, "hooks_sessions_ts_extend", "CONSUMED_AT") ||
      has_edge(res.fragment, backend_health, "hooks_sessions_ts_extend", "CONSUMED_AT")) {
    std::cerr << "discover: a route the consumer serves itself was mapped through the proxy\n";
    return 1;
  }
  if (has_edge(res.fragment, own_route, "hooks_sessions_ts_extend", "CONSUMED_AT") ||
      !has_edge(res.fragment, proxied_own, "hooks_sessions_ts_extend", "CONSUMED_AT")) {
    std::cerr << "discover: a proxied call joined a route only the consumer itself serves\n";
    return 1;
  }
  const bool logged = std::ranges::any_of(res.resolution_log, [](const std::string& line) {
    return line == "prefix web /api/backend -> /api: 1 consumed endpoints joined at the proxied path";
  });
  if (!logged) {
    std::cerr << "discover: the prefix count is not logged\n";
    return 1;
  }

  auto snapshot = [&](const fs::path& path) {
    std::ifstream input(path);
    json graph;
    input >> graph;
    return cgraph::parse_node_link_graph(graph);
  };
  const std::vector<std::pair<std::string, cgraph::GraphSnapshot>> services{{"idp", snapshot(idp_graph)},
                                                                            {"web", snapshot(web_graph)}};
  const auto fused = cgraph::fuse_seam(res.fragment, services, prefixes);
  if (!fused.ok || !has_snapshot_edge(fused.graph, "web::hooks_sessions_ts_extend", provided, "CONSUMES") ||
      has_snapshot_edge(fused.graph, "web::hooks_sessions_ts_extend", proxied, "CONSUMES") ||
      find_in(fused.graph, proxied) != nullptr ||
      !has_snapshot_edge(fused.graph, "web::hooks_sessions_ts_extend", local, "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "web::hooks_sessions_ts_extend", proxied_own, "CONSUMES") ||
      has_snapshot_edge(fused.graph, "web::hooks_sessions_ts_extend", own_route, "CONSUMES")) {
    std::cerr << "fuse: the proxied CONSUMES edge was not redirected to the joined endpoint\n";
    return 1;
  }
  // A seam discovered without the prefix does not carry the joined endpoint
  // when no provider graph is fused: refused, never dangling.
  const auto web_only = cgraph::discover_seam({{"web", web_graph}});
  const auto refused = cgraph::fuse_seam(web_only.fragment, {{"web", snapshot(web_graph)}}, prefixes);
  if (refused.ok || refused.errors.empty()) {
    std::cerr << "fuse: a proxied edge with no joined endpoint was placed instead of refused\n";
    return 1;
  }
  return 0;
}

// One repo's graph built the way the pipeline builds it: function nodes, then
// resolve_contracts over raw `provides_contract` / `uses_contract` facts
// (relation, function, "<kind>:<name>", database), written as node-link JSON.
fs::path contract_graph(const fs::path& root, const std::string& repo,
                        const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& facts) {
  cgraph::GraphSnapshot graph;
  const auto file = repo + "/src/app.ts";
  graph.nodes.push_back({.id = repo + "_src_app_ts", .label = "app.ts", .source_file = file, .kind = "file"});
  std::vector<cgraph::RawRelation> relations;
  std::uint32_t line = 1;
  for (const auto& [relation, function, context, database] : facts) {
    const auto id = repo + "_" + function;
    if (find_in(graph, id) == nullptr) {
      cgraph::Node node{.id = id, .label = function, .source_file = file, .kind = "function"};
      node.source_location = cgraph::SourceLocation{.start_line = line, .end_line = line + 2};
      line += 3;
      graph.nodes.push_back(std::move(node));
      graph.edges.push_back({.source = repo + "_src_app_ts", .target = id, .relation = "contains"});
    }
    relations.push_back(
        {.source_id = id, .target_label = database, .relation = relation, .context = context, .source_file = file});
  }
  cgraph::resolve_contracts(graph, relations);
  const auto path = root / (repo + "-contracts.json");
  write_json(path, cgraph::to_node_link_json(graph));
  return path;
}

// Contracts other than endpoints join across graphs at their raw ids: a header
// read by api and sent by ml (case differs) is one contract. A table no
// database is declared for stays repo-local and joins nobody; once api and ml
// declare database `turing`, theirs join at `table:turing:users` while
// billing's own `users` table still joins nothing. A declared env variable is
// served by its service. Fuse shares the bridged ids and scopes the rest.
int test_generic_contracts(const fs::path& root) {
  const auto api = contract_graph(root, "api",
                                  {{"provides_contract", "createUsers", "table:users", ""},
                                   // A framework-bound read (`@RequestHeader`): it provides
                                   // without a caller (contracts.hpp kBoundHeaderRead).
                                   {"provides_contract", "readTenant", "header:X-Tenant-Id", "bound"},
                                   {"provides_contract", "readTenant", "header:Authorization", "bound"},
                                   {"uses_contract", "readTenant", "env:NODE_ENV", ""}});
  const auto ml = contract_graph(root, "ml",
                                 {{"uses_contract", "listUsers", "table:users", ""},
                                  {"uses_contract", "sendTenant", "header:x-tenant-id", ""},
                                  {"uses_contract", "sendTenant", "env:API_URL", ""},
                                  {"uses_contract", "sendTenant", "header:authorization", ""},
                                  {"uses_contract", "sendTenant", "env:NODE_ENV", ""}});
  const auto billing = contract_graph(root, "billing", {{"uses_contract", "chargeUsers", "table:users", ""}});
  const std::vector<std::pair<std::string, fs::path>> graphs{{"api", api}, {"ml", ml}, {"billing", billing}};

  const auto plain = cgraph::discover_seam(graphs);
  const auto* header = find_node(plain.fragment, "header:x-tenant-id");
  if (!plain.ok || header == nullptr || header->kind != "header" || header->label != "X-Tenant-Id" ||
      header->properties.contains("served") || !has_edge(plain.fragment, "header:x-tenant-id", "api_readTenant", "HANDLED_BY") ||
      !has_edge(plain.fragment, "header:x-tenant-id", "ml_sendTenant", "CONSUMED_AT") ||
      !has_edge(plain.fragment, "header:x-tenant-id", "service:api", "SERVED_BY") ||
      !has_edge(plain.fragment, "service:ml", "header:x-tenant-id", "CONSUMES")) {
    std::cerr << "discover: a header read by one repo and sent by another did not join\n";
    return 1;
  }
  for (const auto& node : plain.fragment.nodes) {
    // Every service reads NODE_ENV and sends Authorization: neither, nor an
    // undeclared env variable or table, is a contract between them.
    if (node.id.starts_with("table:") || node.id.starts_with("env:") || node.id == "header:authorization") {
      std::cerr << "discover: an undeclared table or env name, or a standard header, entered the seam: " << node.id << '\n';
      return 1;
    }
  }
  const auto other_line = std::ranges::any_of(plain.resolution_log, [](const std::string& line) {
    return line.starts_with("other contracts (tables, graph labels, headers, claims, env, DynamoDB tables): matched 1;");
  });
  if (!other_line) {
    std::cerr << "discover: other contracts are not logged\n";
    return 1;
  }

  const std::vector<cgraph::ContractDatabase> databases{{.name = "turing", .repos = {"api", "ml"}}};
  const std::vector<cgraph::EnvProvider> env{{.name = "API_URL", .service = "api"}};
  const auto declared = cgraph::discover_seam(graphs, {}, databases, env);
  const auto* users = find_node(declared.fragment, "table:turing:users");
  if (!declared.ok || users == nullptr || users->kind != "table" || users->properties.contains("served") ||
      users->properties.at("database") != "turing" ||
      !has_edge(declared.fragment, "table:turing:users", "api_createUsers", "HANDLED_BY") ||
      !has_edge(declared.fragment, "table:turing:users", "ml_listUsers", "CONSUMED_AT") ||
      has_edge(declared.fragment, "table:turing:users", "billing_chargeUsers", "CONSUMED_AT") ||
      find_node(declared.fragment, "table:local:users") != nullptr) {
    std::cerr << "discover: members of a declared database did not join at its id, or an outsider did\n";
    return 1;
  }
  if (!has_edge(declared.fragment, "env:API_URL", "service:api", "SERVED_BY") ||
      !has_edge(declared.fragment, "env:API_URL", "ml_sendTenant", "CONSUMED_AT") ||
      find_node(declared.fragment, "env:API_URL")->properties.contains("served") ||
      find_node(declared.fragment, "env:NODE_ENV") != nullptr) {
    std::cerr << "discover: a declared env variable is not served by its service\n";
    return 1;
  }
  const bool database_line = std::ranges::any_of(declared.resolution_log, [](const std::string& line) {
    return line == "database turing (api,ml): 2 repo-local tables and labels joined at the database's id";
  });
  if (!database_line) {
    std::cerr << "discover: the database count is not logged\n";
    return 1;
  }

  auto snapshot = [&](const fs::path& path) {
    std::ifstream input(path);
    json graph;
    input >> graph;
    return cgraph::parse_node_link_graph(graph);
  };
  const std::vector<std::pair<std::string, cgraph::GraphSnapshot>> services{
      {"api", snapshot(api)}, {"ml", snapshot(ml)}, {"billing", snapshot(billing)}};
  const auto fused_plain = cgraph::fuse_seam(plain.fragment, services);
  if (!fused_plain.ok || !has_snapshot_edge(fused_plain.graph, "ml::ml_sendTenant", "header:x-tenant-id", "CONSUMES") ||
      !has_snapshot_edge(fused_plain.graph, "header:x-tenant-id", "api::api_readTenant", "handled_by") ||
      find_in(fused_plain.graph, "ml::header:x-tenant-id") != nullptr || find_in(fused_plain.graph, "table:local:users") != nullptr ||
      find_in(fused_plain.graph, "ml::table:local:users") == nullptr || find_in(fused_plain.graph, "api::table:local:users") == nullptr ||
      find_in(fused_plain.graph, "env:NODE_ENV") != nullptr || find_in(fused_plain.graph, "api::env:NODE_ENV") == nullptr ||
      find_in(fused_plain.graph, "ml::env:NODE_ENV") == nullptr || find_in(fused_plain.graph, "env:API_URL") != nullptr ||
      find_in(fused_plain.graph, "header:authorization") != nullptr ||
      find_in(fused_plain.graph, "ml::header:authorization") == nullptr) {
    std::cerr << "fuse: a header is not shared, or an undeclared table is not scoped to its service\n";
    return 1;
  }
  // Fused without the declarations discover joined under, the join would split
  // (ml's node scoped to `ml::env:API_URL` / `ml::table:local:users` while the
  // seam keeps the shared id): refused, naming the missing flag.
  const auto no_env = cgraph::fuse_seam(declared.fragment, services, {}, databases);
  const auto no_database = cgraph::fuse_seam(declared.fragment, services, {}, {}, env);
  if (no_env.ok || no_env.errors.empty() || no_env.errors.front().find("env:API_URL") == std::string::npos ||
      no_env.errors.front().find("--env") == std::string::npos || no_database.ok || no_database.errors.empty() ||
      no_database.errors.front().find("table:turing:users") == std::string::npos ||
      no_database.errors.front().find("--database") == std::string::npos) {
    std::cerr << "fuse: a seam joined under --env/--database was fused without them and not refused: "
              << (no_env.errors.empty() ? std::string{"(no error)"} : no_env.errors.front()) << " | "
              << (no_database.errors.empty() ? std::string{"(no error)"} : no_database.errors.front()) << '\n';
    return 1;
  }
  const auto fused = cgraph::fuse_seam(declared.fragment, services, {}, databases, env);
  const auto* fused_users = find_in(fused.graph, "table:turing:users");
  if (!fused.ok || fused_users == nullptr || fused_users->properties.at("database") != "turing" ||
      !has_snapshot_edge(fused.graph, "ml::ml_sendTenant", "env:API_URL", "CONSUMES") ||
      find_in(fused.graph, "ml::env:API_URL") != nullptr || find_in(fused.graph, "ml::env:NODE_ENV") == nullptr ||
      !has_snapshot_edge(fused.graph, "ml::ml_listUsers", "table:turing:users", "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "table:turing:users", "api::api_createUsers", "handled_by") ||
      !has_snapshot_edge(fused.graph, "billing::billing_chargeUsers", "billing::table:local:users", "CONSUMES") ||
      find_in(fused.graph, "ml::table:local:users") != nullptr) {
    for (const auto& error : fused.errors) std::cerr << "  " << error << '\n';
    std::cerr << "fuse: a declared member's table is not the database's shared id\n";
    return 1;
  }
  return 0;
}

// A DynamoDB table joins repositories by name with no declaration: api writes
// it and web reads it, both through `process.env.DYNAMODB_TABLE_NAME ||
// 'turing-agents-dev'`, so discover joins them at `dynamo:turing-agents-dev`
// and carries the env variable onto the seam node; agents' table under the
// same variable with another default joins nobody. Fuse shares the id.
int test_dynamo_contracts(const fs::path& root) {
  const auto api = contract_graph(root, "dapi",
                                  {{"provides_contract", "storeTokens", "dynamo:turing-agents-dev", "DYNAMODB_TABLE_NAME"}});
  const auto web = contract_graph(root, "dweb",
                                  {{"uses_contract", "getConnection", "dynamo:turing-agents-dev", "DYNAMODB_TABLE_NAME"},
                                   {"uses_contract", "getConnection", "dynamo:sessions", ""}});
  const auto agents = contract_graph(root, "dagents",
                                     {{"provides_contract", "remember", "dynamo:wiki-agent-memory", "DYNAMODB_TABLE_NAME"}});
  const std::vector<std::pair<std::string, fs::path>> graphs{{"dapi", api}, {"dweb", web}, {"dagents", agents}};
  const auto seam = cgraph::discover_seam(graphs);
  const auto* shared = find_node(seam.fragment, "dynamo:turing-agents-dev");
  if (!seam.ok || shared == nullptr || shared->kind != "dynamo" || shared->properties.contains("served") ||
      !shared->properties.contains("env") || shared->properties.at("env") != "DYNAMODB_TABLE_NAME" ||
      !has_edge(seam.fragment, "dynamo:turing-agents-dev", "dapi_storeTokens", "HANDLED_BY") ||
      !has_edge(seam.fragment, "dynamo:turing-agents-dev", "dweb_getConnection", "CONSUMED_AT") ||
      !has_edge(seam.fragment, "dynamo:turing-agents-dev", "service:dapi", "SERVED_BY") ||
      !has_edge(seam.fragment, "service:dweb", "dynamo:turing-agents-dev", "CONSUMES") ||
      has_edge(seam.fragment, "dynamo:turing-agents-dev", "dagents_remember", "HANDLED_BY") ||
      has_edge(seam.fragment, "dynamo:wiki-agent-memory", "dweb_getConnection", "CONSUMED_AT")) {
    std::cerr << "discover: a DynamoDB table written by one repo and read by another did not join by name\n";
    return 1;
  }
  auto snapshot = [&](const fs::path& path) {
    std::ifstream input(path);
    json graph;
    input >> graph;
    return cgraph::parse_node_link_graph(graph);
  };
  const std::vector<std::pair<std::string, cgraph::GraphSnapshot>> services{
      {"dapi", snapshot(api)}, {"dweb", snapshot(web)}, {"dagents", snapshot(agents)}};
  const auto fused = cgraph::fuse_seam(seam.fragment, services);
  const auto* fused_table = find_in(fused.graph, "dynamo:turing-agents-dev");
  if (!fused.ok || fused_table == nullptr || !fused_table->properties.contains("env") ||
      !has_snapshot_edge(fused.graph, "dweb::dweb_getConnection", "dynamo:turing-agents-dev", "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "dynamo:turing-agents-dev", "dapi::dapi_storeTokens", "handled_by") ||
      find_in(fused.graph, "dweb::dynamo:turing-agents-dev") != nullptr) {
    for (const auto& error : fused.errors) std::cerr << "  " << error << '\n';
    std::cerr << "fuse: a DynamoDB table is not one shared node\n";
    return 1;
  }
  return 0;
}

// Claims under a declared issuer: idp mints the token web and cli read, api
// trusts its own. Undeclared, `session_id` joins idp to api too and `email`
// joins nobody; with `idp=idp,web,cli`, members meet at `claim:idp:<name>`
// (OIDC `email` included, RFC 7519 `sub` never) and api's `session_id` and
// `roles` stay apart from them. Fuse shares the issuer's ids, scopes the
// rest, and refuses a seam joined under an issuer it is not given.
int test_issuer_claims(const fs::path& root) {
  const auto idp = contract_graph(root, "idp",
                                  {{"provides_contract", "mintToken", "claim:session_id", ""},
                                   {"provides_contract", "mintToken", "claim:email", ""},
                                   {"provides_contract", "mintToken", "claim:sub", ""},
                                   {"provides_contract", "mintToken", "claim:roles", ""}});
  const auto web = contract_graph(root, "web",
                                  {{"uses_contract", "readToken", "claim:session_id", ""},
                                   {"uses_contract", "readToken", "claim:email", ""},
                                   {"uses_contract", "readToken", "claim:sub", ""}});
  const auto cli = contract_graph(root, "cli", {{"uses_contract", "whoami", "claim:email", ""}});
  const auto api = contract_graph(root, "api",
                                  {{"uses_contract", "checkToken", "claim:session_id", ""},
                                   {"uses_contract", "checkToken", "claim:roles", ""},
                                   {"uses_contract", "checkToken", "claim:email", ""}});
  const std::vector<std::pair<std::string, fs::path>> graphs{{"idp", idp}, {"web", web}, {"cli", cli}, {"api", api}};

  const auto plain = cgraph::discover_seam(graphs);
  if (!plain.ok || !has_edge(plain.fragment, "claim:session_id", "api_checkToken", "CONSUMED_AT") ||
      !has_edge(plain.fragment, "claim:session_id", "idp_mintToken", "HANDLED_BY") ||
      find_node(plain.fragment, "claim:email") != nullptr) {
    std::cerr << "discover: with no issuer an application claim does not join every repo, or a standard one joins\n";
    return 1;
  }

  const std::vector<cgraph::ClaimIssuer> issuers{{.name = "idp", .repos = {"idp", "web", "cli"}}};
  const auto declared = cgraph::discover_seam(graphs, {}, {}, {}, issuers);
  const auto* email = find_node(declared.fragment, "claim:idp:email");
  if (!declared.ok || email == nullptr || email->kind != "claim" || email->label != "email" ||
      email->properties.contains("served") || email->properties.at("issuer") != "idp" ||
      !has_edge(declared.fragment, "claim:idp:email", "idp_mintToken", "HANDLED_BY") ||
      !has_edge(declared.fragment, "claim:idp:email", "web_readToken", "CONSUMED_AT") ||
      !has_edge(declared.fragment, "claim:idp:email", "cli_whoami", "CONSUMED_AT") ||
      !has_edge(declared.fragment, "claim:idp:session_id", "web_readToken", "CONSUMED_AT") ||
      has_edge(declared.fragment, "claim:idp:session_id", "api_checkToken", "CONSUMED_AT") ||
      !has_edge(declared.fragment, "claim:idp:roles", "service:idp", "SERVED_BY")) {
    std::cerr << "discover: members of a declared issuer did not join at its id, or an outsider did\n";
    return 1;
  }
  for (const auto& node : declared.fragment.nodes) {
    if (node.id == "claim:sub" || node.id == "claim:idp:sub" || node.id == "claim:email") {
      std::cerr << "discover: a registered claim, or an outsider's OIDC claim, entered the seam: " << node.id << '\n';
      return 1;
    }
  }
  // api's own claims stay apart: its `session_id` and `roles` have no provider.
  const auto* api_session = find_node(declared.fragment, "claim:session_id");
  if (api_session == nullptr || !has_edge(declared.fragment, "claim:session_id", "api_checkToken", "CONSUMED_AT") ||
      has_edge(declared.fragment, "claim:session_id", "idp_mintToken", "HANDLED_BY") ||
      api_session->properties.contains("issuer") ||
      has_edge(declared.fragment, "claim:roles", "idp_mintToken", "HANDLED_BY")) {
    std::cerr << "discover: an outsider's claim joined the issuer's members\n";
    return 1;
  }
  const bool issuer_line = std::ranges::any_of(declared.resolution_log, [](const std::string& line) {
    return line == "issuer idp (idp,web,cli): 6 claims joined at the issuer's id";
  });
  const bool no_issuer_line = std::ranges::none_of(plain.resolution_log, [](const std::string& line) {
    return line.starts_with("issuer ");
  });
  if (!issuer_line || !no_issuer_line) {
    for (const auto& line : declared.resolution_log) std::cerr << "  " << line << '\n';
    std::cerr << "discover: the issuer count is not logged, or logged with no issuer\n";
    return 1;
  }

  auto snapshot = [&](const fs::path& path) {
    std::ifstream input(path);
    json graph;
    input >> graph;
    return cgraph::parse_node_link_graph(graph);
  };
  const std::vector<std::pair<std::string, cgraph::GraphSnapshot>> services{
      {"idp", snapshot(idp)}, {"web", snapshot(web)}, {"cli", snapshot(cli)}, {"api", snapshot(api)}};
  const auto undeclared = cgraph::fuse_seam(declared.fragment, services);
  if (undeclared.ok || undeclared.errors.empty() || undeclared.errors.front().find("--issuer") == std::string::npos) {
    std::cerr << "fuse: a seam joined under --issuer was fused without it and not refused: "
              << (undeclared.errors.empty() ? std::string{"(no error)"} : undeclared.errors.front()) << '\n';
    return 1;
  }
  // The reverse: discovered with no issuer, fused with one. idp's
  // `claim:session_id` would be scoped to `claim:idp:session_id` away from the
  // seam's `claim:session_id`: refused too, saying the two runs disagree.
  const auto reverse = cgraph::fuse_seam(plain.fragment, services, {}, {}, {}, issuers);
  if (reverse.ok || reverse.errors.empty() || reverse.errors.front().find("claim:session_id") == std::string::npos ||
      reverse.errors.front().find("discover and fuse were given different --issuer") == std::string::npos) {
    std::cerr << "fuse: a seam discovered without --issuer was fused with one and not refused: "
              << (reverse.errors.empty() ? std::string{"(no error)"} : reverse.errors.front()) << '\n';
    return 1;
  }
  const auto fused = cgraph::fuse_seam(declared.fragment, services, {}, {}, {}, issuers);
  const auto* fused_email = find_in(fused.graph, "claim:idp:email");
  if (!fused.ok || fused_email == nullptr || fused_email->properties.at("issuer") != "idp" ||
      !has_snapshot_edge(fused.graph, "web::web_readToken", "claim:idp:email", "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "cli::cli_whoami", "claim:idp:email", "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "claim:idp:email", "idp::idp_mintToken", "handled_by") ||
      !has_snapshot_edge(fused.graph, "api::api_checkToken", "claim:session_id", "CONSUMES") ||
      !has_snapshot_edge(fused.graph, "api::api_checkToken", "api::claim:email", "CONSUMES") ||
      find_in(fused.graph, "web::claim:sub") == nullptr || find_in(fused.graph, "claim:idp:sub") != nullptr ||
      find_in(fused.graph, "web::claim:email") != nullptr) {
    for (const auto& error : fused.errors) std::cerr << "  " << error << '\n';
    std::cerr << "fuse: an issuer member's claim is not the issuer's shared id, or an outsider's is\n";
    return 1;
  }
  return 0;
}

int main() {
  const auto root = fs::temp_directory_path() / "cgraph-seam-test";
  fs::remove_all(root);

  // Backend consumer graph. The call site at score.ts:42 falls inside BOTH a
  // file-spanning function (1-100) and the precise scoreModel function (40-50);
  // resolution must pick the smaller span. The mirror type lives at types.ts:10.
  const auto backend_graph = root / "backend.json";
  write_json(backend_graph,
             json{{"nodes",
                   {{{"id", "backend::module"},
                     {"label", "score module"},
                     {"type", "file"},
                     {"source_file", "backend/src/score.ts"},
                     {"source_location", {{"start_line", 1}, {"end_line", 100}}}},
                    {{"id", "backend::outer"},
                     {"label", "outer"},
                     {"type", "function"},
                     {"source_file", "backend/src/score.ts"},
                     {"source_location", {{"start_line", 1}, {"end_line", 100}}}},
                    {{"id", "backend::scoreModel"},
                     {"label", "scoreModel"},
                     {"type", "function"},
                     {"source_file", "backend/src/score.ts"},
                     {"source_location", {{"start_line", 40}, {"end_line", 50}}}},
                    {{"id", "backend::ScoreResult"},
                     {"label", "ScoreResult"},
                     {"type", "interface"},
                     {"source_file", "backend/src/types.ts"},
                     {"source_location", {{"start_line", 8}, {"end_line", 12}}}}}}});

  std::unordered_map<std::string, fs::path> graphs{{"backend", backend_graph}};

  // ---- happy path ----
  auto res = cgraph::generate_seam(make_spec(), graphs);
  if (!res.ok || !res.errors.empty()) {
    return 1;
  }
  const auto& frag = res.fragment;

  // Contract nodes present with the expected ids/kinds.
  const auto* provider = find_node(frag, "service:ml-api");
  const auto* endpoint = find_node(frag, "endpoint:ml-api:POST /v3/score");
  const auto* schema = find_node(frag, "schema:ml-api:v3:ScoreResult");
  if (provider == nullptr || provider->kind != "service" ||
      find_node(frag, "service:backend") == nullptr || endpoint == nullptr ||
      endpoint->kind != "endpoint" || schema == nullptr || schema->kind != "schema") {
    return 1;
  }

  // Anchor resolution picked the smallest-span node (scoreModel 40-50, not outer 1-100),
  // and the shadow code-ref carries the consumer node's REAL id.
  const auto* call_ref = find_node(frag, "backend::scoreModel");
  const auto* mirror_ref = find_node(frag, "backend::ScoreResult");
  if (call_ref == nullptr || call_ref->kind != "code-ref" || mirror_ref == nullptr ||
      mirror_ref->kind != "code-ref") {
    return 1;
  }
  if (find_node(frag, "backend::outer") != nullptr) {
    return 1;  // the larger-span node must NOT have been chosen
  }

  // The five contract edges.
  if (!has_edge(frag, "endpoint:ml-api:POST /v3/score", "service:ml-api", "SERVED_BY") ||
      !has_edge(frag, "endpoint:ml-api:POST /v3/score", "schema:ml-api:v3:ScoreResult",
                "RESPONDS_WITH") ||
      !has_edge(frag, "service:backend", "endpoint:ml-api:POST /v3/score", "CONSUMES") ||
      !has_edge(frag, "endpoint:ml-api:POST /v3/score", "backend::scoreModel", "CONSUMED_AT") ||
      !has_edge(frag, "schema:ml-api:v3:ScoreResult", "backend::ScoreResult", "MIRRORED_BY")) {
    return 1;
  }

  // The emitted fragment must be ingestable through the existing validation path.
  if (!cgraph::validate_semantic_fragment_json(cgraph::to_json(frag)).valid) {
    return 1;
  }

  // Byte-stable: regenerating from the same spec + graphs yields an identical
  // serialization (emission order is deterministic, not hash-ordered).
  auto res2 = cgraph::generate_seam(make_spec(), graphs);
  if (!res2.ok || cgraph::to_json(res2.fragment).dump() != cgraph::to_json(frag).dump()) {
    return 1;
  }

  // ---- fail-loud error paths: each must return ok=false and an empty fragment ----
  auto expect_fail = [&](json spec, std::unordered_map<std::string, fs::path> g) -> bool {
    auto r = cgraph::generate_seam(spec, g);
    return !r.ok && !r.errors.empty() && r.fragment.nodes.empty() && r.fragment.edges.empty();
  };

  // Unresolved anchor (no node spans line 999).
  json bad_anchor = make_spec();
  bad_anchor["consumes"][0]["call_site"]["line"] = 999;
  if (!expect_fail(bad_anchor, graphs)) {
    return 1;
  }
  // consumes references an endpoint that was never declared.
  json bad_endpoint = make_spec();
  bad_endpoint["consumes"][0]["path"] = "/v3/nope";
  if (!expect_fail(bad_endpoint, graphs)) {
    return 1;
  }
  // mirror references an undeclared schema.
  json bad_schema = make_spec();
  bad_schema["mirrors"][0]["schema"] = "Ghost";
  if (!expect_fail(bad_schema, graphs)) {
    return 1;
  }
  // call_site names a graph that was not supplied.
  if (!expect_fail(make_spec(), {})) {
    return 1;
  }
  // malformed spec: missing a required top-level field.
  json malformed = make_spec();
  malformed.erase("provider");
  if (!expect_fail(malformed, graphs)) {
    return 1;
  }

  // ---- fuse: merge the seam fragment with the real service graph into a view ----
  // The backend service graph carries the REAL nodes the seam's shadows reference.
  cgraph::GraphSnapshot backend;
  backend.nodes.push_back(cgraph::Node{.id = "backend::scoreModel", .label = "scoreModel",
                                       .kind = "function"});
  backend.nodes.push_back(cgraph::Node{.id = "backend::ScoreResult", .label = "ScoreResult",
                                       .kind = "interface"});
  backend.nodes.push_back(cgraph::Node{.id = "backend::helper", .label = "helper",
                                       .kind = "function"});
  backend.edges.push_back(
      cgraph::Edge{.source = "backend::scoreModel", .target = "backend::helper", .relation = "CALLS"});

  auto fused = cgraph::fuse_seam(frag, {{"backend", backend}});
  if (!fused.ok) {
    return 1;
  }
  // Every backend node is tagged with its service community and scoped by its
  // service (`backend::` + its own id); the call site is the REAL node (kind
  // function), not a dropped code-ref shadow.
  const auto* score = find_in(fused.graph, "backend::backend::scoreModel");
  if (score == nullptr || score->kind != "function" ||
      score->properties.at("community") != "backend") {
    return 1;
  }
  // No code-ref shadow survives the fuse.
  for (const auto& node : fused.graph.nodes) {
    if (node.kind == "code-ref") {
      return 1;
    }
  }
  // Seam contract nodes cluster with their service / provider.
  const auto* svc = find_in(fused.graph, "service:backend");
  const auto* ep = find_in(fused.graph, "endpoint:ml-api:POST /v3/score");
  if (svc == nullptr || svc->properties.at("community") != "backend" || ep == nullptr ||
      ep->properties.at("community") != "ml-api") {
    return 1;
  }
  // The CONSUMED_AT contract edge now binds to the real backend node, and the
  // backend's own CALLS edge survived the merge.
  if (!has_snapshot_edge(fused.graph, "endpoint:ml-api:POST /v3/score", "backend::backend::scoreModel",
                         "CONSUMED_AT") ||
      !has_snapshot_edge(fused.graph, "backend::backend::scoreModel", "backend::backend::helper", "CALLS")) {
    return 1;
  }

  // Fail loud: omitting the backend service graph leaves CONSUMED_AT/MIRRORED_BY
  // edges dangling -> no fused graph.
  auto dangling = cgraph::fuse_seam(frag, {});
  if (dangling.ok || dangling.errors.empty() || !dangling.graph.nodes.empty()) {
    return 1;
  }

  // ---- the fused seam graph is queryable cross-service (the path `seam query` drives) ----
  cgraph::DaemonState qstate;
  cgraph::publish_graph_snapshot(qstate, std::move(fused.graph));

  // impact of the schema reaches the endpoint (RESPONDS_WITH) and the consuming
  // service (CONSUMES) -- a cross-service blast radius.
  const auto impact = cgraph::handle_daemon_request(
      qstate, cgraph::make_request("impact", {{"id", "schema:ml-api:v3:ScoreResult"},
                                              {"direction", "dependents"}, {"max_depth", 3}}));
  bool reaches_endpoint = false;
  for (const auto& node : impact["result"]["nodes"]) {
    if (node.value("id", std::string{}) == "endpoint:ml-api:POST /v3/score") {
      reaches_endpoint = true;
    }
  }
  if (!impact["ok"].get<bool>() || !reaches_endpoint) {
    return 1;
  }

  // path from a backend call site to the ml-api endpoint resolves across the seam.
  const auto path = cgraph::handle_daemon_request(
      qstate, cgraph::make_request("path", {{"source", "backend::backend::scoreModel"},
                                            {"target", "endpoint:ml-api:POST /v3/score"}}));
  if (!path["ok"].get<bool>() || path["result"]["path"].size() < 2) {
    return 1;
  }

  // explain the endpoint: its cross-service neighbors (CONSUMES/SERVED_BY/RESPONDS_WITH).
  const auto explain = cgraph::handle_daemon_request(
      qstate, cgraph::make_request("explain", {{"id", "endpoint:ml-api:POST /v3/score"}}));
  if (!explain["ok"].get<bool>() || explain["result"].value("neighbor_count", 0U) < 3U) {
    return 1;
  }

  // a write op against a seam (no memory_dir) is rejected, not silently accepted.
  const auto write = cgraph::handle_daemon_request(
      qstate, cgraph::make_request("remember", {{"title", "x"}, {"body", "y"}}));
  if (write.value("ok", true)) {
    return 1;
  }

  // is_seam_directory: true only when the marker is present.
  const auto seamdir = root / "seamview";
  fs::create_directories(seamdir);
  if (cgraph::is_seam_directory(seamdir)) {
    return 1;  // no marker yet
  }
  std::ofstream(seamdir / std::string(cgraph::kSeamMarkerFile)) << "x";
  if (!cgraph::is_seam_directory(seamdir)) {
    return 1;  // marker present
  }

  if (test_discover(root) != 0) {
    return 1;
  }
  if (test_fuse_same_relative_file() != 0) {
    return 1;
  }
  if (test_shared_raw_id(root) != 0) {
    return 1;
  }
  if (test_proxy_prefix(root) != 0) {
    return 1;
  }
  if (test_dynamo_contracts(root) != 0) {
    return 1;
  }
  if (test_generic_contracts(root) != 0) {
    return 1;
  }
  if (test_issuer_claims(root) != 0) {
    return 1;
  }

  fs::remove_all(root);
  return 0;
}
