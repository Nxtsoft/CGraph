#pragma once

#include "cgraph/daemon_identity.hpp"

#include "cgraph/workspace.hpp"

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
  // "nothing depends on this". For a workspace it bounds the whole federated
  // request, not each member. Zero returns the building answer immediately;
  // status, update, shutdown and remember never wait.
  std::chrono::milliseconds build_wait{30000};
  // Whether this request may federate. A root inside a workspace member makes
  // `impact` and `path` cross the workspace; asks forwarded to member repos set
  // this false so a member never re-federates (which would recurse).
  bool federate = true;
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

// The other repos of the workspace enclosing `request.project_root`, with an ask
// that reaches each one's daemon (one shared build wait, never re-federating).
// Empty when the root is in no workspace.
struct CrossServiceScope {
  EnclosingWorkspace enclosing;
  RepoAsk ask;
};
[[nodiscard]] std::optional<CrossServiceScope> cross_service_scope_for(const ClientRequest& request,
                                                                       ClientRuntimeHooks hooks);

// change_context over `parameters`, adding the `cross_service` section when the
// target root sits in a workspace member. The CLI and the MCP server both call
// this, so both answer the same. Throws as change_context does.
[[nodiscard]] nlohmann::json change_context_across_workspace(const nlohmann::json& parameters,
                                                             const ClientRequest& base);

// Before a file is edited: the endpoints it serves (declares) and calls, and
// the other services on the far side of each, from the workspace enclosing
// `base.project_root`. Carries `summary`, plain lines an agent can read, empty
// when nothing crosses a service boundary; `workspace` is null outside one.
[[nodiscard]] nlohmann::json cross_service_for_file(const ClientRequest& base, const std::filesystem::path& file);

// A Claude Code PreToolUse hook for Edit, Write and MultiEdit: given the hook's
// stdin JSON, the hook's stdout JSON (`hookSpecificOutput.additionalContext`
// listing the other services behind the file's endpoints), or nothing when the
// file is outside a workspace, touches no endpoint another service uses, or
// cannot be read. Never blocks an edit. `wait` bounds how long cold daemons
// are waited for.
[[nodiscard]] std::optional<nlohmann::json> pre_edit_hook_output(const nlohmann::json& hook_input,
                                                                 const ClientRequest& base,
                                                                 std::chrono::milliseconds wait);

}  // namespace cgraph
