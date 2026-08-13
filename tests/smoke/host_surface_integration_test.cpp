#include "cgraph/daemon_ops.hpp"
#include "cgraph/mcp_server.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/semantic_chunk_plan.hpp"
#include "cgraph/semantic_drop.hpp"
#include "cgraph/semantic_fragment_validation.hpp"
#include "cgraph/semantic_ingest.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool has_node_label(const cgraph::GraphSnapshot& graph, std::string_view label) {
  for (const auto& node : graph.nodes) {
    if (node.label == label) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return 1;
  }

  const auto hook = read_file(argv[1]);
  const auto always_on = read_file(argv[2]);
  if (hook.empty() || always_on.empty()) {
    return 1;
  }

  if (!contains(hook, "query|path|explain|update|status|shutdown") ||
      !contains(hook, "--root \"${CGRAPH_PROJECT_ROOT}\"") ||
      !contains(hook, "--daemon \"${CGRAPH_DAEMON}\"") ||
      !contains(always_on, "run_client update") ||
      !contains(always_on, "run_client status") ||
      !contains(always_on, "if [ \"${CGRAPH_ONCE}\" = \"1\" ]; then\n  run_client status\n  exit $?\nfi") ||
      !contains(always_on, "CGRAPH_ONCE") ||
      !contains(always_on, "--daemon \"${CGRAPH_DAEMON}\"")) {
    return 1;
  }

  // weakly_canonical: the planner canonicalizes root internally, so planned
  // input paths are canonical; build the comparison root the same way.
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path() / "cgraph-host-surface-integration-test");
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto source = root / "docs" / "guide.md";
  const auto drop_dir = root / "graphify-out" / "semantic-drop";
  const auto fragment_path = drop_dir / "chunk_00.json";
  write_file(source, "# Guide\nDescribes search routing.\n");

  cgraph::SemanticCache cache;
  const auto plan = cgraph::plan_semantic_chunks(root, cache, {.max_files_per_chunk = 1});
  if (plan.chunks.size() != 1 || plan.chunks.front().index != 0 || plan.chunks.front().inputs.size() != 1 ||
      plan.chunks.front().inputs.front().path != source || plan.cache_hits != 0 || plan.stale_inputs != 0) {
    return 1;
  }

  write_file(
      fragment_path,
      R"({
        "nodes": [
          {"id": "doc:guide", "label": "Guide", "type": "document", "source_file": "docs/guide.md"},
          {"id": "concept:routing", "label": "Routing", "type": "concept", "source_file": "docs/guide.md"}
        ],
        "edges": [
          {"source": "doc:guide", "target": "concept:routing", "relation": "MENTIONS"}
        ],
        "hyperedges": []
      })");

  const auto drops = cgraph::discover_semantic_fragment_drops(drop_dir);
  if (drops.size() != 1 || drops.front().chunk_index != 0 || drops.front().path != fragment_path) {
    return 1;
  }

  const auto validation = cgraph::validate_semantic_fragment_file(fragment_path);
  if (!validation.valid || !validation.errors.empty()) {
    return 1;
  }

  cgraph::DaemonState state;
  cgraph::GraphSnapshot initial;
  initial.nodes.push_back(cgraph::Node{.id = "code:router", .label = "Router", .kind = "class"});
  initial.build_state = cgraph::BuildState::DeterministicReady;
  cgraph::publish_graph_snapshot(state, std::move(initial));

  state.enrichment_state = cgraph::EnrichmentState::Running;
  state.enrichment_running = plan.chunks.size();
  const auto ingest = cgraph::ingest_semantic_fragment(
      state, cache, {{.path = source}}, fragment_path);
  if (!ingest.merged || !ingest.cache_updated || !ingest.errors.empty()) {
    return 1;
  }

  state.enrichment_state = cgraph::EnrichmentState::Idle;
  state.enrichment_running = 0;
  const auto graph = cgraph::read_graph_snapshot(state);
  if (!has_node_label(*graph, "Router") || !has_node_label(*graph, "Guide") || !has_node_label(*graph, "Routing")) {
    return 1;
  }

  const auto cache_hit_plan = cgraph::plan_semantic_chunks(root, cache, {.max_files_per_chunk = 1});
  if (!cache_hit_plan.chunks.empty() || cache_hit_plan.cache_hits != 1 || cache_hit_plan.stale_inputs != 0) {
    return 1;
  }

  nlohmann::json forwarded;
  const auto response = cgraph::handle_mcp_request(
      nlohmann::json{
          {"jsonrpc", "2.0"},
          {"id", 7},
          {"method", "tools/call"},
          {"params", {{"name", "graph_query"}, {"arguments", {{"query", "Guide"}}}}}},
      [&](const nlohmann::json& daemon_request) {
        forwarded = daemon_request;
        return cgraph::handle_daemon_request(state, daemon_request);
      });

  if (forwarded["op"] != "query" || forwarded["params"]["query"] != "Guide" ||
      !contains(response["result"]["content"][0]["text"].get<std::string>(), "Guide")) {
    return 1;
  }

  std::filesystem::remove_all(root);
  return 0;
}
