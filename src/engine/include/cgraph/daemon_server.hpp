#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>

namespace cgraph {

struct DaemonServerOptions {
  // Exit after this much inactivity so idle per-project daemons don't linger.
  std::chrono::seconds idle_timeout{300};
  // When false, the graph is built synchronously before the socket is accepted
  // on (the default); tests can disable to serve an empty graph immediately.
  bool build_graph_on_start = true;
  // Directory the daemon watches for host-dropped chunk_NN.json fragments.
  // Empty -> default_semantic_drop_dir(root / "cgraph-out").
  std::filesystem::path drop_dir;
  // How often the serve loop polls the drop directory for new fragments.
  std::chrono::milliseconds drop_poll_interval{200};
  // Live code watching: poll the project tree for changed source files and fold
  // them into the graph incrementally, no `update` op needed. Each poll walks
  // the tree (real cost on a large repo), so it runs on its own, slower cadence
  // than the drop poll. 0 disables watching. Watching only starts once the
  // initial build has published a baseline graph.
  std::chrono::milliseconds code_poll_interval{2000};
  // A save burst on one file coalesces into a single event.
  std::chrono::milliseconds watch_debounce{250};
  // More pending file events than this collapse into one full rescan (e.g. a
  // git checkout or branch switch touching hundreds of files).
  std::size_t watch_max_pending = 256;
  // How long incremental graph changes may stay memory-only before the serve
  // loop re-persists graph.json + the index manifest (crash/restart safety).
  std::chrono::seconds persist_interval{30};
};

// Runs the per-project daemon: builds the deterministic graph for `root`, binds
// the per-root Unix socket endpoint, and serves length-prefixed JSON-frame
// requests (query/path/explain/update/status/shutdown) until a shutdown op or
// the idle timeout. Returns 0 on clean exit, non-zero on a setup failure.
[[nodiscard]] int run_daemon_server(const std::filesystem::path& root, DaemonServerOptions options = {});

// Runs a static, read-only daemon over a pre-fused seam graph (`root/graph.json`):
// serves the read ops from the loaded snapshot with no build, watcher, persistence,
// or enrichment; `update` reloads the graph, writes are rejected. graphd selects
// this when `root` carries the seam marker (is_seam_directory). Returns 0 on clean
// exit, non-zero on a setup/load failure.
[[nodiscard]] int run_static_seam_server(const std::filesystem::path& root,
                                         DaemonServerOptions options = {});

// Connects to a daemon's Unix socket, sends one request frame, and returns the
// decoded response. Returns nullopt if the socket is absent/unreachable or the
// exchange fails — the signal the thin client uses to spawn and retry. With a
// `deadline`, nothing blocks past it: the connect does not wait for a full
// backlog, and a daemon that has not answered by then (busy, stopped) yields
// nullopt at the deadline.
[[nodiscard]] std::optional<nlohmann::json> request_over_unix_socket(
    const std::filesystem::path& socket_path,
    const nlohmann::json& request,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

}  // namespace cgraph
