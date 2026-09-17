#include "cgraph/daemon_lifecycle.hpp"

#include "cgraph/export_json.hpp"
#include "cgraph/fragment_json.hpp"

#include <fstream>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <system_error>

namespace cgraph {
namespace {

}  // namespace

void record_daemon_activity(DaemonLifecycleState& lifecycle, DaemonClock::time_point now) {
  lifecycle.last_activity = now;
}

void mark_graph_dirty(DaemonLifecycleState& lifecycle, DaemonClock::time_point now) {
  // dirty_since anchors to the FIRST unpersisted change: re-marking on every
  // subsequent edit must not push the persist deadline out, or a steady edit
  // stream would starve persistence indefinitely.
  if (!lifecycle.graph_dirty) {
    lifecycle.dirty_since = now;
  }
  lifecycle.graph_dirty = true;
}

bool should_shutdown_for_idle(
    const DaemonLifecycleState& lifecycle,
    const DaemonLifecycleConfig& config,
    DaemonClock::time_point now) {
  // A non-positive idle timeout means "never idle-shut-down": a supervised /
  // resident daemon stays alive until an explicit shutdown op or signal, so the
  // background watcher keeps folding edits in whether or not queries arrive.
  if (config.idle_timeout <= std::chrono::seconds::zero()) {
    return false;
  }
  return now - lifecycle.last_activity >= config.idle_timeout;
}

bool cleanup_daemon_endpoint(const std::filesystem::path& endpoint_path) {
  if (endpoint_path.empty()) {
    return true;
  }
  std::error_code error;
  std::filesystem::remove(endpoint_path, error);
  return !error;
}

namespace {

// Function fingerprints live beside graph.json, never inside it: graph.json is
// the Graphify-parity export and stays byte-identical. Absent or unreadable,
// the fast-loaded graph simply has no fingerprints until the next rescan; the
// clones report says so rather than failing.
[[nodiscard]] std::filesystem::path fingerprints_path(const std::filesystem::path& graph_path) {
  return graph_path.parent_path() / "fingerprints.json";
}

[[nodiscard]] bool write_atomically(const std::filesystem::path& path, const std::string& contents) {
  const auto temp_path = path.parent_path() / (path.filename().string() + ".tmp");
  {
    std::ofstream output(temp_path);
    if (!output) {
      return false;
    }
    output << contents;
  }
  std::error_code error;
  std::filesystem::rename(temp_path, path, error);
  if (error) {
    std::error_code cleanup;
    std::filesystem::remove(temp_path, cleanup);
    return false;
  }
  return true;
}

void persist_fingerprints(const GraphSnapshot& snapshot, const std::filesystem::path& graph_path) {
  nlohmann::json functions = nlohmann::json::object();
  for (const auto& [id, fingerprint] : snapshot.fingerprints) {
    functions[id] = {{"tokens", fingerprint.tokens}, {"shingles", fingerprint.shingles}};
  }
  const nlohmann::json document{{"version", 1}, {"functions", std::move(functions)}};
  (void)write_atomically(fingerprints_path(graph_path), document.dump());
}

[[nodiscard]] std::unordered_map<std::string, FunctionFingerprint> load_fingerprints(const std::filesystem::path& graph_path) {
  std::unordered_map<std::string, FunctionFingerprint> out;
  std::ifstream input(fingerprints_path(graph_path));
  if (!input) {
    return out;
  }
  try {
    const auto document = nlohmann::json::parse(input);
    if (document.value("version", 0) != 1 || !document.contains("functions") || !document["functions"].is_object()) {
      return out;
    }
    for (const auto& [id, entry] : document["functions"].items()) {
      FunctionFingerprint fingerprint;
      fingerprint.tokens = entry.value("tokens", std::uint32_t{0});
      fingerprint.shingles = entry.value("shingles", std::vector<std::uint64_t>{});
      out.emplace(id, std::move(fingerprint));
    }
  } catch (const nlohmann::json::exception&) {
    out.clear();
  }
  return out;
}

}  // namespace

bool persist_graph_snapshot(
    const GraphSnapshot& snapshot,
    const std::filesystem::path& graph_path) {
  if (graph_path.empty()) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(graph_path.parent_path(), error);
  if (error) {
    return false;
  }

  const auto temp_path = graph_path.parent_path() / (graph_path.filename().string() + ".tmp");
  {
    std::ofstream output(temp_path);
    if (!output) {
      return false;
    }
    output << to_node_link_json(snapshot).dump(2) << '\n';
  }

  // Atomic replace only. rename() over an existing file is atomic on POSIX, so
  // graph.json is never observed missing or half-written. On failure we must NOT
  // delete the existing last-known-good graph.json to "make room" for a retry:
  // if the retry also failed the daemon would be left with no graph at all. Leave
  // the prior file untouched, remove the orphan temp, and surface the failure.
  std::filesystem::rename(temp_path, graph_path, error);
  if (error) {
    std::error_code cleanup;
    std::filesystem::remove(temp_path, cleanup);  // drop the orphan temp; keep the good file
    return false;
  }
  persist_fingerprints(snapshot, graph_path);
  return true;
}

bool persist_graph_snapshot(const DaemonState& state, const std::filesystem::path& graph_path) {
  // Direct in-process callers do not own the daemon's explicit deterministic
  // snapshot. Preserve the established memory-sidecar contract for that narrow
  // API; the daemon itself always calls the GraphSnapshot overload with its
  // code-only persistence snapshot, which excludes every overlay by construction.
  auto snapshot = *read_graph_snapshot(state);
  std::erase_if(snapshot.nodes, [](const Node& node) { return is_memory_node_id(node.id); });
  std::erase_if(snapshot.edges, [](const Edge& edge) {
    return is_memory_node_id(edge.source) || is_memory_node_id(edge.target);
  });
  return persist_graph_snapshot(snapshot, graph_path);
}

bool load_graph_snapshot(DaemonState& state, const std::filesystem::path& graph_path) {
  std::ifstream input(graph_path);
  if (!input) {
    return false;
  }

  try {
    const auto json = nlohmann::json::parse(input);
    auto graph = parse_node_link_graph(json);
    graph.fingerprints = load_fingerprints(graph_path);
    publish_graph_snapshot(state, std::move(graph));
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  return true;
}

bool persist_if_due(
    const GraphSnapshot& persistence_snapshot,
    DaemonLifecycleState& lifecycle,
    const DaemonLifecycleConfig& config,
    DaemonClock::time_point now) {
  if (!lifecycle.graph_dirty || now - lifecycle.dirty_since < config.persist_interval) {
    return false;
  }
  if (!persist_graph_snapshot(persistence_snapshot, config.graph_path)) {
    return false;
  }
  lifecycle.graph_dirty = false;
  lifecycle.last_persist = now;
  return true;
}

}  // namespace cgraph
