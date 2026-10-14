#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cgraph {

// launchd label for the single supervisor agent that reconciles per-repo daemons.
inline constexpr const char* kSupervisorLabel = "com.cgraph.supervisor";
// Prefix for each per-repo daemon agent: kPerRepoLabelPrefix + root_hash.
inline constexpr const char* kPerRepoLabelPrefix = "com.cgraph.graphd.";

struct LaunchAgentSpec {
  std::string label;
  std::vector<std::string> program_args;   // program_args[0] is the absolute binary path
  bool keep_alive = false;                 // launchd restarts the process if it exits
  bool run_at_load = false;                // start at load / login
  std::optional<int> start_interval;       // seconds between relaunches (supervisor only)
  std::filesystem::path working_directory; // optional; empty -> omitted
  std::filesystem::path log_path;          // optional; StandardOut+ErrorPath when set
  // EnvironmentVariables entries; empty -> dict omitted. launchd starts agents
  // with a minimal environment, so anything a program needs beyond PATH-less
  // absolute paths must be baked in here at install time.
  std::vector<std::pair<std::string, std::string>> env;
};

// Render a launchd LaunchAgent plist (XML) for the given spec. Pure string
// function — no filesystem or launchctl side effects.
[[nodiscard]] std::string render_launch_agent(const LaunchAgentSpec& spec);

// The managed label for a tracked repo's daemon agent.
[[nodiscard]] std::string per_repo_agent_label(const std::string& root_hash);

// The root hash embedded in a managed per-repo label, or empty if the label is
// not a managed per-repo agent.
[[nodiscard]] std::string root_hash_from_label(const std::string& label);

// ~/Library/LaunchAgents (macOS). Empty if $HOME is unset.
[[nodiscard]] std::filesystem::path default_launch_agents_dir();

// Write plist to launch_agents_dir/<label>.plist. Returns the written path, or
// empty on failure.
[[nodiscard]] std::filesystem::path write_launch_agent(
    const std::filesystem::path& launch_agents_dir, const LaunchAgentSpec& spec);

// Run argv to completion without a shell (no quoting pitfalls), inheriting the
// environment. Returns the exit status, or -1 if it could not be spawned or did
// not exit normally. macOS and Linux only (launchctl / systemctl glue).
#if defined(__APPLE__) || defined(__linux__)
[[nodiscard]] int run_service_command(const std::vector<std::string>& argv);
#endif

// launchctl glue (macOS). bootstrap loads+starts a plist into the user gui domain;
// bootout stops+unloads a label. Both return false on non-macOS or on error.
[[nodiscard]] bool launchctl_bootstrap(const std::filesystem::path& plist_path);
[[nodiscard]] bool launchctl_bootout(const std::string& label);

// True when `label` is currently registered in the user gui domain (probed via
// `launchctl print gui/<uid>/<label>` exit status). False on non-macOS, or when
// the service is not loaded ("Could not find service").
[[nodiscard]] bool launchctl_is_loaded(const std::string& label);

}  // namespace cgraph
