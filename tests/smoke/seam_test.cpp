#include "cgraph/seam.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/fragment_json.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_fragment_validation.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

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

  const auto res = cgraph::discover_seam({{"api", api_graph}, {"web", web_graph}});
  if (!res.ok || !res.errors.empty()) {
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
  const auto fused = cgraph::fuse_seam(frag, {{"api", api_snapshot}, {"web", web_snapshot}});
  if (!fused.ok || find_in(fused.graph, starred) == nullptr ||
      !has_snapshot_edge(fused.graph, "service:web", starred, "CONSUMES")) {
    return 1;
  }
  // Byte-stable regeneration.
  const auto again = cgraph::discover_seam({{"api", api_graph}, {"web", web_graph}});
  if (!again.ok || cgraph::to_json(again.fragment).dump() != cgraph::to_json(frag).dump()) {
    return 1;
  }
  // A missing graph is a hard error.
  if (cgraph::discover_seam({{"api", root / "nope.json"}}).ok) {
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
  // Every backend node is tagged with its service community; the call site is the
  // REAL node (kind function), not a dropped code-ref shadow.
  const auto* score = find_in(fused.graph, "backend::scoreModel");
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
  if (!has_snapshot_edge(fused.graph, "endpoint:ml-api:POST /v3/score", "backend::scoreModel",
                         "CONSUMED_AT") ||
      !has_snapshot_edge(fused.graph, "backend::scoreModel", "backend::helper", "CALLS")) {
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
      qstate, cgraph::make_request("path", {{"source", "backend::scoreModel"},
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

  fs::remove_all(root);
  return 0;
}
