#pragma once

#include "cgraph/launch_agent.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cgraph {

// The supervisor runs on the platform's per-user service manager: launchd
// LaunchAgents (<label>.plist) on macOS, systemd --user units (<label>.service,
// plus <label>.timer for the periodic supervisor) on Linux. Labels are the same
// on both, so discovery, reconcile and the socket identity do not change.
#if defined(__linux__)
inline constexpr const char* kUserServiceExtension = ".service";
#else
inline constexpr const char* kUserServiceExtension = ".plist";
#endif

// ~/Library/LaunchAgents on macOS; $XDG_CONFIG_HOME/systemd/user (default
// ~/.config/systemd/user) on Linux.
[[nodiscard]] std::filesystem::path default_user_service_dir();

// The definition file `label` is stored under in dir.
[[nodiscard]] std::filesystem::path user_service_file(const std::filesystem::path& dir, const std::string& label);

// Write the platform definition for spec into dir without loading it. Returns
// the definition path, or empty on failure.
[[nodiscard]] std::filesystem::path write_user_service(const std::filesystem::path& dir, const LaunchAgentSpec& spec);

// A repo cgraph tracks for background refresh: a project root whose .mcp.json
// registers a cgraph MCP server. Keyed by the same canonical-root hash the daemon
// uses for its endpoint identity, so the discovery key, the LaunchAgent label, and
// the socket name all agree.
struct TrackedRepo {
  std::filesystem::path root;  // canonical project root
  std::string root_hash;       // stable_project_hash(root)
};

// Where the supervisor looks, and the binaries it wires into LaunchAgents.
struct SupervisorConfig {
  std::vector<std::filesystem::path> search_roots;  // scanned: each dir + its immediate children
  std::vector<std::filesystem::path> exclude;       // roots to skip (canonicalized on compare)
  // The subset of search_roots / exclude given as --search-root / --exclude flags.
  // Only these are written into the supervisor's scheduled `daemon sync`; roots
  // from supervisor.json are re-read by that sync, so edits to the file apply.
  std::vector<std::filesystem::path> flag_search_roots;
  std::vector<std::filesystem::path> flag_exclude;
  std::filesystem::path launch_agents_dir;          // where service definitions are written (default_user_service_dir())
  std::filesystem::path graphd_binary;              // absolute path to graphd (per-repo agents)
  std::filesystem::path cgraph_binary;              // absolute path to cgraph (supervisor agent runs `daemon sync`)
  int reconcile_interval_seconds = 300;             // supervisor StartInterval
};

// True if mcp_json parses and registers an `mcpServers.cgraph` entry.
[[nodiscard]] bool mcp_json_registers_cgraph(const std::filesystem::path& mcp_json);

// Scan each search root (the dir itself and its immediate children) for a
// .mcp.json that registers cgraph. Returns tracked repos sorted by root_hash,
// deduped, with `exclude` roots removed. Deterministic for a given tree.
[[nodiscard]] std::vector<TrackedRepo> discover_tracked_repos(const SupervisorConfig& config);

// Pure set difference keyed by root_hash: which discovered repos need an agent
// started (to_add) and which installed agent hashes are no longer discovered
// (to_remove). Idempotent — identical inputs yield empty diffs.
struct ReconcilePlan {
  std::vector<TrackedRepo> to_add;
  std::vector<std::string> to_remove;  // root hashes
};
[[nodiscard]] ReconcilePlan reconcile_tracked_repos(
    const std::vector<TrackedRepo>& discovered,
    const std::vector<std::string>& installed_hashes);

// Root hashes of the managed per-repo services currently present in
// launch_agents_dir (parsed from com.cgraph.graphd.<hash><kUserServiceExtension>).
[[nodiscard]] std::vector<std::string> installed_managed_hashes(
    const std::filesystem::path& launch_agents_dir);

// What the reconcile pass should do about the enrichment drainer's residency.
// Installation is explicit (`cgraph drain install`); reconcile only restores an
// already-installed drainer whose service has fallen out of the launchd domain.
enum class DrainerResidencyAction {
  kNone,          // plist absent (never installed) -> reconcile leaves it alone
  kRebootstrap,   // plist present but service not loaded -> re-bootstrap it
};

// Pure decision: given whether the drainer plist exists in launch_agents_dir and
// whether its service is currently loaded in the launchd domain, decide the action.
// Directly testable with no launchctl side effects.
[[nodiscard]] DrainerResidencyAction decide_drainer_residency(bool plist_exists, bool service_loaded);

// Reconcile the managed per-repo agents against discovery. When apply is true,
// each to_add gets a plist written + bootstrapped and each to_remove gets booted
// out + its plist removed. Returns the plan it acted on (for inspection/tests).
struct SupervisorSyncResult {
  ReconcilePlan plan;
  std::vector<std::string> failed;  // labels that failed to bootstrap/bootout
  bool drainer_rebootstrapped = false;  // reconcile restored an unloaded installed drainer
};
[[nodiscard]] SupervisorSyncResult supervisor_sync(const SupervisorConfig& config, bool apply);

// Write + bootstrap the supervisor agent (which runs `cgraph daemon sync` at load
// and every reconcile_interval_seconds), then run one sync. Returns false on error.
[[nodiscard]] bool supervisor_install(const SupervisorConfig& config);

// The supervisor's service: runs `cgraph daemon sync` plus the flag roots and
// excludes (absolute), at load and every reconcile_interval_seconds.
[[nodiscard]] LaunchAgentSpec supervisor_spec(const SupervisorConfig& config);

// The labels uninstall removes, in order: the supervisor first (so a scheduled
// sync cannot re-add daemons mid-uninstall), then every managed per-repo daemon
// whose definition is in launch_agents_dir.
[[nodiscard]] std::vector<std::string> supervisor_uninstall_labels(const SupervisorConfig& config);

// Boot out + remove the supervisor agent and every managed per-repo agent.
// Reverses supervisor_install / supervisor_sync — leaves no managed plist behind.
[[nodiscard]] bool supervisor_uninstall(const SupervisorConfig& config);

// Per-repo liveness for the `status` subcommand.
struct SupervisorRepoStatus {
  TrackedRepo repo;
  bool daemon_live = false;  // a daemon is currently listening on the repo's socket
};
[[nodiscard]] std::vector<SupervisorRepoStatus> supervisor_status(const SupervisorConfig& config);

}  // namespace cgraph
