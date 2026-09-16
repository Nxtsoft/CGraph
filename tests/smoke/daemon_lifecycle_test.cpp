#include "cgraph/daemon_lifecycle.hpp"

#include "cgraph/daemon_ops.hpp"

#include <chrono>
#include <filesystem>
#include <cstdint>
#include <vector>
#include <iterator>
#include <fstream>

namespace {

cgraph::GraphSnapshot sample_graph() {
  cgraph::GraphSnapshot graph;
  graph.nodes.push_back(cgraph::Node{.id = "a", .label = "Alpha", .source_file = "a.cpp",
                                     .source_location = cgraph::SourceLocation{.start_line = 10,
                                                                               .start_column = 1,
                                                                               .end_line = 25,
                                                                               .end_column = 2},
                                     .kind = "class"});
  graph.nodes.push_back(cgraph::Node{.id = "b", .label = "Beta", .source_file = "b.cpp", .kind = "function"});
  graph.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "CALLS"});
  graph.build_state = cgraph::BuildState::DeterministicReady;
  graph.cache_hit_rate = 0.5;
  return graph;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  const auto root = std::filesystem::temp_directory_path() / "cgraph-daemon-lifecycle-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  cgraph::DaemonState state;
  const auto deterministic = sample_graph();
  auto fused = deterministic;
  fused.nodes.push_back(cgraph::Node{.id = "doc:overlay", .label = "Overlay", .kind = "document"});
  fused.nodes.push_back(cgraph::Node{.id = "memory:checkpoint", .label = "Checkpoint", .kind = "memory"});
  fused.edges.push_back(cgraph::Edge{.source = "doc:overlay", .target = "a", .relation = "MENTIONS"});
  fused.edges.push_back(cgraph::Edge{.source = "memory:checkpoint", .target = "a", .relation = "concerns"});
  cgraph::publish_graph_snapshot(state, std::move(fused));

  const auto graph_path = root / "graph.json";
  // Function fingerprints ride in a sidecar, never in graph.json (report-clones).
  auto with_fingerprints = deterministic;
  with_fingerprints.fingerprints["b"] = cgraph::FunctionFingerprint{.shingles = {3, 7, 11}, .tokens = 42};
  if (!cgraph::persist_graph_snapshot(with_fingerprints, graph_path)) {
    return 1;
  }
  {
    std::ifstream graph_file(graph_path);
    const std::string graph_text((std::istreambuf_iterator<char>(graph_file)), std::istreambuf_iterator<char>());
    if (graph_text.find("fingerprint") != std::string::npos || !std::filesystem::exists(root / "fingerprints.json")) {
      return 1;  // graph.json stays the parity export; the sidecar exists beside it
    }
  }

  cgraph::DaemonState reloaded;
  if (!cgraph::load_graph_snapshot(reloaded, graph_path)) {
    return 1;
  }
  const auto snapshot = cgraph::read_graph_snapshot(reloaded);
  if (snapshot->nodes.size() != 2 || snapshot->edges.size() != 1 ||
      snapshot->build_state != cgraph::BuildState::DeterministicReady || snapshot->cache_hit_rate != 0.5) {
    return 1;
  }
  if (const auto fp = snapshot->fingerprints.find("b");
      fp == snapshot->fingerprints.end() || fp->second.tokens != 42 || fp->second.shingles != std::vector<std::uint64_t>{3, 7, 11}) {
    return 1;  // fingerprints survive persist -> load
  }
  // An older persist has no sidecar: the graph still loads, with no fingerprints.
  std::filesystem::remove(root / "fingerprints.json");
  cgraph::DaemonState older;
  if (!cgraph::load_graph_snapshot(older, graph_path) || !cgraph::read_graph_snapshot(older)->fingerprints.empty()) {
    return 1;
  }

  // Regression: the source span must survive persist -> load. Without it a
  // fast-loaded daemon serves location-less, snippet-less context and starves
  // the slice-cost knapsack packer (research/2510.00446).
  const cgraph::Node* alpha = nullptr;
  for (const auto& node : snapshot->nodes) {
    if (node.id == "a") {
      alpha = &node;
    }
  }
  if (alpha == nullptr || !alpha->source_location || alpha->source_location->start_line != 10 ||
      alpha->source_location->end_line != 25 || alpha->source_location->end_column != 2) {
    return 1;
  }

  const auto endpoint = root / "graphd.sock";
  {
    std::ofstream output(endpoint);
    output << "stale endpoint";
  }
  if (!std::filesystem::exists(endpoint) || !cgraph::cleanup_daemon_endpoint(endpoint) ||
      std::filesystem::exists(endpoint) || !cgraph::cleanup_daemon_endpoint(endpoint)) {
    return 1;
  }

  cgraph::DaemonLifecycleConfig config{
      .endpoint_path = endpoint,
      .graph_path = graph_path,
      .idle_timeout = 30s,
      .persist_interval = 10s,
  };
  cgraph::DaemonLifecycleState lifecycle;
  const auto start = cgraph::DaemonClock::time_point{} + 100s;
  cgraph::record_daemon_activity(lifecycle, start);
  if (cgraph::should_shutdown_for_idle(lifecycle, config, start + 29s)) {
    return 1;
  }
  if (!cgraph::should_shutdown_for_idle(lifecycle, config, start + 30s)) {
    return 1;
  }

  // A non-positive idle timeout means "never idle-shut-down" (resident/supervised
  // mode): should_shutdown_for_idle stays false no matter how much time elapses.
  cgraph::DaemonLifecycleConfig immortal_config{
      .endpoint_path = endpoint,
      .graph_path = graph_path,
      .idle_timeout = std::chrono::seconds::zero(),
      .persist_interval = 10s,
  };
  cgraph::DaemonLifecycleState immortal_lifecycle;
  cgraph::record_daemon_activity(immortal_lifecycle, start);
  if (cgraph::should_shutdown_for_idle(immortal_lifecycle, immortal_config, start + 100000s)) {
    return 1;
  }

  cgraph::mark_graph_dirty(lifecycle, start + 1s);
  if (cgraph::persist_if_due(deterministic, lifecycle, config, start + 9s)) {
    return 1;
  }
  // Re-marking while already dirty must NOT push the deadline out: dirty_since
  // anchors to the first unpersisted change, so a steady edit stream still
  // persists once the interval elapses.
  cgraph::mark_graph_dirty(lifecycle, start + 10s);
  if (lifecycle.dirty_since != start + 1s) {
    return 1;
  }
  if (!cgraph::persist_if_due(deterministic, lifecycle, config, start + 11s) || lifecycle.graph_dirty) {
    return 1;
  }
  if (cgraph::persist_if_due(deterministic, lifecycle, config, start + 12s)) {
    return 1;
  }
  // After a persist, the next mark re-anchors dirty_since.
  cgraph::mark_graph_dirty(lifecycle, start + 20s);
  if (lifecycle.dirty_since != start + 20s || !lifecycle.graph_dirty) {
    return 1;
  }

  // Durability: a failed atomic rename must NOT destroy the existing
  // last-known-good graph. Force rename() to fail by making the destination a
  // non-empty directory (rename of the temp FILE over a directory fails on
  // POSIX). persist must return false, leave that destination untouched (no
  // "remove then retry" that could wipe the good graph), and clean up the temp
  // file it wrote. Regression coverage for the old destructive fallback.
  const auto blocked_path = root / "blocked-graph.json";
  std::filesystem::create_directories(blocked_path);           // destination is a directory...
  std::ofstream(blocked_path / "keep.txt") << "last known good";  // ...and non-empty
  const auto temp_path = root / "blocked-graph.json.tmp";
  if (cgraph::persist_graph_snapshot(deterministic, blocked_path)) {
    return 1;  // must fail: cannot atomically replace a non-empty directory
  }
  if (!std::filesystem::is_directory(blocked_path) ||
      !std::filesystem::exists(blocked_path / "keep.txt")) {
    return 1;  // the prior data must survive the failed persist untouched
  }
  if (std::filesystem::exists(temp_path)) {
    return 1;  // the orphan temp must be cleaned up, not left behind
  }

  std::filesystem::remove_all(root);
  return 0;
}
