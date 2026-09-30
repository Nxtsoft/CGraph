#include "cgraph/langgraph_config.hpp"

#include "cgraph/pipeline.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int fail(std::string_view message) {
  std::cerr << "langgraph_config_test: " << message << '\n';
  return 1;
}

constexpr std::string_view kConfig = R"json({
  "node_version": "20",
  "dependencies": ["."],
  "graphs": {
    "luna": "./src/luna_agent/graph.ts:graph",
    "compiq": { "path": "./src/compiq_agent/graph.ts:graph", "description": "comp IQ" }
  },
  "env": ".env"
})json";

const cgraph::Node* node_of(const cgraph::Fragment& fragment, std::string_view kind, std::string_view label) {
  for (const auto& node : fragment.nodes) {
    if (node.kind == kind && node.label == label) {
      return &node;
    }
  }
  return nullptr;
}

bool has_route(const cgraph::ExtractionResult& result, std::string_view context) {
  return std::ranges::any_of(result.raw_relations, [&](const cgraph::RawRelation& relation) {
    return relation.relation == "file_route" && relation.context == context;
  });
}

// The config declares a server and its graphs; every server route is a
// `file_route` fact handled by the server node.
int test_extracts_server_graphs_and_routes() {
  const auto result = cgraph::extract_langgraph_config(
      {.source_file = "/repo/langgraph.json", .relative_path = "langgraph.json", .source = kConfig});
  const auto* server = node_of(result.fragment, "langgraph_server", "LangGraph server");
  if (server == nullptr) {
    return fail("no langgraph_server node");
  }
  if (!server->source_location || server->source_location->start_line != 4 || server->source_location->end_line != 7) {
    return fail("server node should span the graphs object, lines 4-7");
  }
  const auto* luna = node_of(result.fragment, "langgraph_graph", "luna");
  const auto* compiq = node_of(result.fragment, "langgraph_graph", "compiq");
  if (luna == nullptr || compiq == nullptr) {
    return fail("one langgraph_graph node per graphs entry");
  }
  if (!luna->source_location || luna->source_location->start_line != 5 || compiq->source_location->start_line != 6) {
    return fail("graph nodes anchor on their own lines (5, 6)");
  }
  if (luna->properties.at("entrypoint") != "./src/luna_agent/graph.ts:graph" ||
      compiq->properties.at("entrypoint") != "./src/compiq_agent/graph.ts:graph") {
    return fail("entrypoint property from the string and the object form");
  }
  // 49 served routes: the four crons routes answer 500 "Not implemented" in
  // @langchain/langgraph-api 1.5.1, so they are not served.
  if (cgraph::langgraph_server_routes().size() != 49 || has_route(result, "post /runs/crons") ||
      has_route(result, "post /threads/:thread_id/runs/crons")) {
    return fail("the unimplemented crons routes are not served");
  }
  if (result.raw_relations.size() != cgraph::langgraph_server_routes().size()) {
    return fail("one file_route per server route, got " + std::to_string(result.raw_relations.size()));
  }
  for (const auto& relation : result.raw_relations) {
    if (relation.source_id != server->id) {
      return fail("every route is handled by the server node");
    }
  }
  for (const auto* route : {"post /runs", "post /runs/wait", "post /runs/stream", "post /threads/:thread_id/runs/stream",
                            "get /assistants/:assistant_id", "put /store/items", "get /ok"}) {
    if (!has_route(result, route)) {
      return fail(std::string("missing route ") + route);
    }
  }
  return 0;
}

// `http.disable_<group>` switches a route group off, as the server does.
int test_disabled_group_is_not_served() {
  const auto result = cgraph::extract_langgraph_config(
      {.source_file = "/repo/langgraph.json", .relative_path = "langgraph.json",
       .source = R"json({"graphs": {"a": "./a.ts:graph"}, "http": {"disable_store": true, "disable_runs": false}})json"});
  if (has_route(result, "put /store/items") || has_route(result, "post /store/items/search")) {
    return fail("disable_store must drop the store routes");
  }
  if (!has_route(result, "post /runs/wait")) {
    return fail("disable_runs:false keeps the runs routes");
  }
  return 0;
}

// `http.mount_prefix` moves the Python server's surface under the prefix, with
// `GET /ok` still at the root; a malformed prefix is a warning and no routes; the
// JS server (node_version) does not read it.
int test_mount_prefix() {
  const auto extract = [](std::string_view source) {
    return cgraph::extract_langgraph_config(
        {.source_file = "/repo/langgraph.json", .relative_path = "langgraph.json", .source = source});
  };
  const auto mounted = extract(R"json({"python_version": "3.12", "graphs": {"a": "./a.py:graph"},
                                       "http": {"mount_prefix": "/my-deployment/api"}})json");
  if (!has_route(mounted, "post /my-deployment/api/runs/wait") ||
      !has_route(mounted, "get /my-deployment/api/assistants/:assistant_id") || has_route(mounted, "post /runs/wait") ||
      !has_route(mounted, "get /my-deployment/api/ok") || !has_route(mounted, "get /ok") ||
      mounted.raw_relations.size() != cgraph::langgraph_server_routes().size() + 1 || !mounted.fragment.warnings.empty()) {
    return fail("mount_prefix prefixes every route and keeps GET /ok at the root");
  }
  for (const std::string_view bad : {std::string_view{R"json({"graphs": {"a": "./a.py:graph"}, "http": {"mount_prefix": "api"}})json"},
                                     std::string_view{R"json({"graphs": {"a": "./a.py:graph"}, "http": {"mount_prefix": "/api/"}})json"},
                                     std::string_view{R"json({"graphs": {"a": "./a.py:graph"}, "http": {"mount_prefix": 7}})json"}}) {
    const auto result = extract(bad);
    if (!result.raw_relations.empty() || result.fragment.warnings.size() != 1 ||
        result.fragment.warnings.front().find("invalid http.mount_prefix") == std::string::npos) {
      return fail("a malformed mount_prefix is a warning and no routes: " + std::string(bad));
    }
  }
  const auto js = extract(R"json({"node_version": "20", "graphs": {"a": "./a.ts:graph"}, "http": {"mount_prefix": "/api"}})json");
  if (!has_route(js, "post /runs/wait") || has_route(js, "post /api/runs/wait") || js.fragment.warnings.size() != 1) {
    return fail("the JS server ignores mount_prefix, with a warning");
  }
  return 0;
}

// Without graphs there is no server: the file node only.
int test_no_graphs_declares_no_server() {
  for (const std::string_view source : {std::string_view{R"json({"graphs": {}})json"}, std::string_view{R"json({"node_version": "20"})json"},
                                        std::string_view{"{ not json"}}) {
    const auto result = cgraph::extract_langgraph_config(
        {.source_file = "/repo/langgraph.json", .relative_path = "langgraph.json", .source = source});
    if (result.fragment.nodes.size() != 1 || result.fragment.nodes.front().kind != "file" || !result.raw_relations.empty()) {
      return fail("a config without graphs yields its file node only: " + std::string(source));
    }
  }
  return 0;
}

// End to end through the pipeline: detection picks up langgraph.json, and
// resolve_contracts mints served endpoints handled by the server node, which a
// client call in the same repo consumes rather than minting a served:false copy.
int test_pipeline_serves_endpoints() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_langgraph_config_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "src");
  std::ofstream(root / "langgraph.json") << kConfig;
  std::ofstream(root / "src" / "client.ts")
      << "export async function startRun() {\n"
         "  const agentUrl = process.env.AGENTS_API_URL;\n"
         "  return fetch(`${agentUrl}/runs/wait`, { method: 'POST' });\n"
         "}\n";
  const auto result = cgraph::run_one_shot(root);
  std::filesystem::remove_all(root);
  const auto& graph = result.graph;
  const cgraph::Node* endpoint = nullptr;
  const cgraph::Node* server = nullptr;
  for (const auto& node : graph.nodes) {
    if (node.id == "endpoint:POST /runs/wait") {
      endpoint = &node;
    }
    if (node.kind == "langgraph_server") {
      server = &node;
    }
  }
  if (endpoint == nullptr || server == nullptr) {
    return fail("pipeline did not mint endpoint:POST /runs/wait and the server node");
  }
  if (endpoint->properties.contains("served")) {
    return fail("the server serves /runs/wait; it must not be a served:false placeholder");
  }
  const auto has = [&](std::string_view source, std::string_view target, std::string_view relation) {
    return std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
      return edge.source == source && edge.target == target && edge.relation == relation;
    });
  };
  if (!has(endpoint->id, server->id, "handled_by")) {
    return fail("endpoint:POST /runs/wait must be handled_by the LangGraph server node");
  }
  const bool consumed = std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
    return edge.target == endpoint->id && edge.relation == "CONSUMES";
  });
  if (!consumed) {
    return fail("the in-repo fetch should consume the served endpoint");
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_extracts_server_graphs_and_routes();
  failures += test_disabled_group_is_not_served();
  failures += test_mount_prefix();
  failures += test_no_graphs_declares_no_server();
  failures += test_pipeline_serves_endpoints();
  return failures == 0 ? 0 : 1;
}
