#include "cgraph/enrich_drainer.hpp"

#include "cgraph/platform.hpp"

#include <string>
#include <system_error>

namespace cgraph {
namespace {

const std::filesystem::path kScriptRelative =
    std::filesystem::path("integrations") / "always-on" / "cgraph-enrich-drain.sh";

[[nodiscard]] std::filesystem::path walk_up_for_script(std::filesystem::path start) {
  std::error_code ec;
  start = std::filesystem::weakly_canonical(start, ec);
  for (auto dir = start; !dir.empty(); dir = dir.parent_path()) {
    if (const auto candidate = dir / kScriptRelative;
        std::filesystem::is_regular_file(candidate, ec)) {
      return candidate;
    }
    if (dir == dir.parent_path()) {
      break;
    }
  }
  return {};
}

[[nodiscard]] std::filesystem::path plist_path(const DrainerConfig& config) {
  return config.launch_agents_dir / (std::string(kDrainerLabel) + ".plist");
}

}  // namespace

std::filesystem::path resolve_drain_script(const std::filesystem::path& executable,
                                           const std::filesystem::path& cwd) {
  if (auto found = walk_up_for_script(executable.parent_path()); !found.empty()) {
    return found;
  }
  return walk_up_for_script(cwd);
}

LaunchAgentSpec drainer_spec(const DrainerConfig& config) {
  LaunchAgentSpec spec;
  spec.label = kDrainerLabel;
  // /bin/sh <script> <cgraph> <cgraph-client> <chunk-cap>: launchd offers no
  // inherited PATH, so every binary the script needs is passed absolute. The
  // script owns host-CLI choice and model routing (host-skill contract).
  spec.program_args = {"/bin/sh", config.script_path.string(), config.cgraph_binary.string(),
                       config.client_binary.string(), std::to_string(config.chunk_cap)};
  spec.keep_alive = false;  // one-shot sweep, not a resident process
  spec.run_at_load = true;
  spec.start_interval = config.interval_seconds;
  // Sweep decisions (which repos, gate skips, drain failures) must be auditable:
  // launchd swallows stdout/stderr unless a log path is set.
  if (const auto home = home_directory(); !home.empty()) {
    spec.log_path = home / "Library" / "Logs" / "cgraph-enrich-drain.log";
  }
  if (!config.host_cli.empty()) {
    spec.env.emplace_back("CGRAPH_HOST_CLI", config.host_cli);
  }
  return spec;
}

std::filesystem::path resolve_host_cli(const std::string& name) {
  std::error_code ec;
  if (const char* override_cli = std::getenv("CGRAPH_HOST_CLI");
      override_cli != nullptr && override_cli[0] != '\0') {
    if (const std::filesystem::path candidate(override_cli);
        std::filesystem::is_regular_file(candidate, ec)) {
      return candidate;
    }
  }
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }
  std::string remaining(path_env);
  while (!remaining.empty()) {
    const auto sep = remaining.find(':');
    const std::string dir = remaining.substr(0, sep);
    remaining = sep == std::string::npos ? std::string{} : remaining.substr(sep + 1);
    if (dir.empty()) {
      continue;
    }
    const auto candidate = std::filesystem::path(dir) / name;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

bool drainer_installed(const DrainerConfig& config) {
  std::error_code ec;
  return std::filesystem::is_regular_file(plist_path(config), ec);
}

bool drainer_install(const DrainerConfig& config) {
  const auto written = write_launch_agent(config.launch_agents_dir, drainer_spec(config));
  if (written.empty()) {
    return false;
  }
  return launchctl_bootstrap(written);
}

bool drainer_uninstall(const DrainerConfig& config) {
  (void)launchctl_bootout(kDrainerLabel);  // best-effort stop; plist removal is what matters
  std::error_code ec;
  std::filesystem::remove(plist_path(config), ec);
  return !drainer_installed(config);
}

}  // namespace cgraph
