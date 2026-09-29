#pragma once

#include "cgraph/daemon_identity.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace cgraph {

struct ClientRequest {
  std::filesystem::path project_root;
  std::string operation;
  nlohmann::json params = nlohmann::json::object();
  std::filesystem::path daemon_path;
  int max_connect_attempts = 8;
  std::chrono::milliseconds initial_backoff{10};
  // How long a graph-reading op (query, path, explain, impact, context, report,
  // recall) keeps re-asking a daemon whose graph is still building. A cold
  // daemon answers at once from an empty graph, and an empty answer reads as
  // "nothing depends on this". Zero returns the building answer immediately;
  // status, update, shutdown and remember never wait.
  std::chrono::milliseconds build_wait{30000};
};

struct ClientResult {
  std::optional<nlohmann::json> response;
  bool spawned = false;
  int connect_attempts = 0;
  std::string error;
};

struct ClientRuntimeHooks {
  std::function<std::optional<nlohmann::json>(const DaemonIdentity&, const nlohmann::json&)> connect;
  std::function<bool(const DaemonIdentity&)> spawn;
  std::function<void(std::chrono::milliseconds)> sleep;
};

// Locate the graphd binary for auto-spawn: an explicit path wins, then the
// CGRAPH_DAEMON_PATH environment variable, then graphd next to the running
// executable (installed layout) or in the build tree's daemon/ directory.
// Returns an empty path when nothing is found.
[[nodiscard]] std::filesystem::path resolve_daemon_path(const std::filesystem::path& requested);

[[nodiscard]] ClientRuntimeHooks default_client_runtime_hooks(const ClientRequest& request);
[[nodiscard]] ClientResult send_thin_client_request(const ClientRequest& request, ClientRuntimeHooks hooks);

}  // namespace cgraph
