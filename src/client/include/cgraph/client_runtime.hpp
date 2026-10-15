#pragma once

#include "cgraph/daemon_identity.hpp"

#include "cgraph/workspace.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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
  // Whether a request that finds no daemon may start one. The pre-edit hook
  // never does: starting a daemon builds a graph, which no single edit can
  // wait for, and writes cgraph-out/ into the repository.
  bool spawn = true;
  // When set, no exchange with a daemon (this request's or a federated
  // member's) runs past it: a busy or stopped daemon counts as unreachable.
  std::optional<std::chrono::steady_clock::time_point> deadline;
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
[[nodiscard]] nlohmann::json cross_service_for_file(const ClientRequest& base, const std::filesystem::path& file,
                                                    std::size_t max_contracts = 24);

// How many of a file's endpoints the pre-edit hook asks about: each is one ask
// per other repository, and the hook runs before every edit.
inline constexpr std::size_t kHookMaxContracts = 8;

// The most the pre-edit hook adds to the agent's context per edit, in the
// ~4 characters per token estimate the report and context ops pack against.
inline constexpr std::size_t kHookTokenBudget = 800;

// How long the pre-edit hook may take, end to end, unless CGRAPH_HOOK_WAIT_MS
// says otherwise: it runs before every edit.
inline constexpr std::chrono::milliseconds kHookDefaultBudget{250};

// The pre-edit hook's message for `file` (root-relative): the files section
// (`neighbors`, `<path>: <reason>` lines) and, when `found` (cross_service_for_file)
// has summary lines, the services section, within kHookTokenBudget. Service
// rows outrank files; rows that do not fit are counted in one closing line,
// files that do not fit are left out. Empty when there is nothing to say.
[[nodiscard]] std::string pre_edit_context(const nlohmann::json& found, const std::string& file,
                                           const std::vector<std::string>& neighbors);

// A Claude Code PreToolUse hook for Edit, Write and MultiEdit: given the hook's
// stdin JSON, the hook's stdout JSON (`hookSpecificOutput.additionalContext`),
// or nothing when there is nothing to say. The context names the files most
// likely to change with the edited lines (the daemon's `co-change` op over the
// file's repository: the nearest `.git`, or the workspace member holding the
// file) and, inside a workspace, the other services behind the file's
// endpoints; the whole message stays within kHookTokenBudget. Only daemons
// already running are asked (none is started, no build is waited for), and
// everything shares one `budget`: when it runs out the hook says nothing.
// Silent for a file outside any repository, one the graph does not know, an
// edit whose text is not in the file, or while the graph is still building
// (the services section still says so). Never blocks an edit.
[[nodiscard]] std::optional<nlohmann::json> pre_edit_hook_output(const nlohmann::json& hook_input,
                                                                 const ClientRequest& base,
                                                                 std::chrono::milliseconds budget);

// The files a Read or Bash tool call names, in order and without repeats: Read's
// `file_path`, or each token of a Bash `command` that, resolved against the hook's
// `cwd`, is an existing regular file. Agents mostly read and edit through Bash
// (`sed -n`, `cat`, `python3` scripts), not the Edit tool.
[[nodiscard]] std::vector<std::filesystem::path> files_named_by_tool(const nlohmann::json& hook_input);

// Most files a read hook reports per tool call; the rest wait for a later call.
inline constexpr std::size_t kReadHookFilesPerCall = 2;

// A Claude Code PostToolUse hook for Read and Bash: for up to
// kReadHookFilesPerCall files the call names (files_named_by_tool) that this
// session has not been told about yet, the same message pre_edit_hook_output
// gives before a whole-file edit, so the agent learns which other files change
// with the one it is looking at while it still explores. Each file is reported
// once per `session_id`: the reported paths are kept in
// `<root>/cgraph-out/read-hook-<session_id>.txt`. Same rules otherwise: only
// running daemons are asked, one `budget` for everything, silent when there is
// nothing to say.
[[nodiscard]] std::optional<nlohmann::json> read_hook_output(const nlohmann::json& hook_input,
                                                             const ClientRequest& base,
                                                             std::chrono::milliseconds budget);

}  // namespace cgraph
