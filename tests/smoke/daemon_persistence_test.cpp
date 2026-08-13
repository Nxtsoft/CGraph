#include "cgraph/daemon_endpoint.hpp"
#include "cgraph/daemon_identity.hpp"
#include "cgraph/daemon_server.hpp"
#include "cgraph/index_persistence.hpp"
#include "cgraph/protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::optional<nlohmann::json> request_with_retry(const std::filesystem::path& socket_path, const nlohmann::json& req) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (auto response = cgraph::request_over_unix_socket(socket_path, req)) {
      return response;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return std::nullopt;
}

// Runs a daemon on its own thread until the graph has at least `min_nodes`
// nodes, returns the node count, then shuts it down cleanly.
int run_until_built(const std::filesystem::path& root, const std::filesystem::path& socket_path, int min_nodes) {
  cgraph::DaemonServerOptions options;
  options.idle_timeout = std::chrono::seconds(10);
  options.build_graph_on_start = true;
  int rc = -1;
  std::thread server([&] { rc = cgraph::run_daemon_server(root, options); });

  int nodes = 0;
  for (int attempt = 0; attempt < 300 && nodes < min_nodes; ++attempt) {
    const auto status = request_with_retry(socket_path, cgraph::make_request("status"));
    nodes = status ? (*status)["result"].value("node_count", 0) : 0;
    if (nodes < min_nodes) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  (void)cgraph::request_over_unix_socket(socket_path, cgraph::make_request("shutdown"));
  server.join();
  (void)rc;
  return nodes;
}

// Adds a recognizable sentinel node to a persisted graph.json. If a later
// daemon start serves this node, it must have loaded the graph from disk — a
// rebuild from source could never produce it.
void inject_sentinel(const std::filesystem::path& graph_path, const std::string& id) {
  auto graph = nlohmann::json::parse(std::ifstream(graph_path));
  graph["nodes"].push_back({{"id", id}, {"label", id}, {"kind", "marker"}});
  std::ofstream(graph_path, std::ios::binary | std::ios::trunc) << graph.dump();
}

bool serves_node(const std::filesystem::path& socket_path, const std::string& query) {
  const auto response = request_with_retry(socket_path, cgraph::make_request("query", {{"q", query}}));
  return response && (*response)["ok"] == true && !(*response)["result"]["nodes"].empty();
}

}  // namespace

// A restart over an unchanged tree must load the persisted graph from disk
// rather than re-extracting; a source change must invalidate the cache and
// force a rebuild. No mocks: the real Unix-socket daemon, real persistence.
int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "cgraph_daemon_persistence_test";
  fs::remove_all(root);
  write_file(root / "src" / "alpha.ts", "export function alpha() { return 1; }\n");

  const auto identity = cgraph::daemon_identity_for(root);
  const auto socket_path = cgraph::unix_socket_path(identity);
  fs::remove(socket_path);
  const auto graph_path = root / "cgraph-out" / "graph.json";
  const auto manifest_path = root / "cgraph-out" / "index-manifest.json";

  bool ok = true;
  const auto expect = [&](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ok = false;
    }
  };

  // First run builds the graph and must persist graph.json + the manifest.
  const auto nodes_first = run_until_built(root, socket_path, 2);
  expect(nodes_first >= 2, "first run built the graph");
  expect(fs::exists(graph_path) && fs::exists(manifest_path),
         "first run persisted graph and manifest");
  const auto persisted_manifest = cgraph::read_index_manifest(manifest_path);
  expect(persisted_manifest.has_value() &&
             persisted_manifest->content_root.algorithm == "sha256-merkle-v1" &&
             persisted_manifest->content_root.sha256.size() == 64,
         "manifest contains a valid Merkle root");
  const auto persisted_root = persisted_manifest ? persisted_manifest->content_root.sha256 : std::string{};

  // Tamper the persisted graph with a sentinel, then restart over the unchanged
  // tree: the daemon must serve the sentinel (proving a disk load, not a rebuild).
  fs::remove(socket_path);
  inject_sentinel(graph_path, "marker:tier1");
  {
    cgraph::DaemonServerOptions options;
    options.idle_timeout = std::chrono::seconds(10);
    options.build_graph_on_start = true;
    options.persist_interval = std::chrono::seconds::zero();
    int rc = -1;
    std::thread server([&] { rc = cgraph::run_daemon_server(root, options); });
    bool found = false;
    for (int attempt = 0; attempt < 300 && !found; ++attempt) {
      found = serves_node(socket_path, "marker:tier1");
      if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(found, "unchanged restart served the fast-loaded sentinel");
    const auto loaded_status = request_with_retry(socket_path, cgraph::make_request("status"));
    expect(loaded_status && (*loaded_status).value("ok", false) &&
               (*loaded_status)["result"]["freshness"].value("content_root", std::string{}) == persisted_root,
           "fast-loaded status retained the persisted Merkle root");

    // Session memory is a live overlay backed by its sidecar. It must be visible
    // immediately without rewriting graph.json or its deterministic manifest.
    const auto graph_before_remember = read_file(graph_path);
    const auto manifest_before_remember = read_file(manifest_path);
    const auto remembered = request_with_retry(
        socket_path,
        cgraph::make_request("remember", {{"title", "fast-load cache"}, {"body", "retain verified leaves"}}));
    const auto recalled = request_with_retry(socket_path, cgraph::make_request("recall"));
    expect(remembered && (*remembered).value("ok", false) && recalled &&
               (*recalled).value("ok", false) &&
               (*recalled)["result"].value("total", 0) == 1 &&
               (*recalled)["result"]["checkpoints"][0].value("label", std::string{}) ==
                   "fast-load cache",
           "session checkpoint is visible in the live overlay");
    // A status round trip enters the next serve-loop iteration, where a wrongly
    // dirtied deterministic snapshot would be persisted immediately (interval=0).
    const auto after_remember = request_with_retry(socket_path, cgraph::make_request("status"));
    const auto unchanged_manifest = cgraph::read_index_manifest(manifest_path);
    expect(after_remember && (*after_remember).value("ok", false) &&
               read_file(graph_path) == graph_before_remember &&
               read_file(manifest_path) == manifest_before_remember &&
               unchanged_manifest.has_value() && unchanged_manifest->files.size() == 1 &&
               unchanged_manifest->content_root.sha256 == persisted_root,
           "session checkpoint left deterministic persistence byte-for-byte unchanged");
    (void)cgraph::request_over_unix_socket(socket_path, cgraph::make_request("shutdown"));
    server.join();
    (void)rc;
  }

  // Change a source file, then restart: the version/stat diff must reject the
  // cache and rebuild from source, so the tampered sentinel disappears and the
  // real symbol is present.
  fs::remove(socket_path);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));  // ensure a new mtime
  write_file(root / "src" / "alpha.ts", "export function alpha() { return 42; }\nexport function beta() { return 2; }\n");
  inject_sentinel(graph_path, "marker:stale");  // still present in the now-stale graph.json
  {
    cgraph::DaemonServerOptions options;
    options.idle_timeout = std::chrono::seconds(10);
    options.build_graph_on_start = true;
    int rc = -1;
    std::thread server([&] { rc = cgraph::run_daemon_server(root, options); });
    bool beta_built = false;
    for (int attempt = 0; attempt < 300 && !beta_built; ++attempt) {
      beta_built = serves_node(socket_path, "beta");
      if (!beta_built) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(beta_built, "changed source rebuilt the graph");
    expect(!serves_node(socket_path, "marker:stale"), "changed source discarded the stale disk graph");
    (void)cgraph::request_over_unix_socket(socket_path, cgraph::make_request("shutdown"));
    server.join();
    (void)rc;
  }

  fs::remove_all(root);
  return ok ? 0 : 1;
}
