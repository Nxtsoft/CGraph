#include "cgraph/daemon_supervisor.hpp"

#include "cgraph/daemon_endpoint.hpp"
#include "cgraph/daemon_identity.hpp"
#include "cgraph/enrich_drainer.hpp"
#include "cgraph/launch_agent.hpp"
#include "cgraph/systemd_unit.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace cgraph {
namespace {

LaunchAgentSpec per_repo_spec(const SupervisorConfig& config, const TrackedRepo& repo) {
  LaunchAgentSpec spec;
  spec.label = per_repo_agent_label(repo.root_hash);
  // --idle-timeout 0 => the daemon never idle-shuts-down (resident/immortal).
  spec.program_args = {
      config.graphd_binary.string(), "--root", repo.root.string(), "--idle-timeout", "0"};
  spec.keep_alive = true;   // launchd restarts it on crash
  spec.run_at_load = true;  // and starts it at login
  return spec;
}

// Load (and start) a definition write_user_service wrote: launchctl bootstrap on
// macOS, systemctl --user enable --now on Linux.
bool load_user_service(const std::filesystem::path& dir, const std::string& label) {
#if defined(__linux__)
  return systemctl_user_enable(dir, label);
#else
  return launchctl_bootstrap(user_service_file(dir, label));
#endif
}

// Stop the service and remove its definition. False when a definition file
// survives, or (Linux) when systemd could not stop a unit this install owns.
// launchd bootout stays best effort: a never-loaded agent has nothing to stop.
bool unload_and_remove_user_service(const std::filesystem::path& dir, const std::string& label) {
#if defined(__linux__)
  // Only a definition this install wrote in the user manager's own directory is
  // stopped: a same-named unit known to the manager from anywhere else belongs
  // to a different install, so a definition elsewhere (tests, a custom
  // --launch-agents-dir) is only deleted, and an absent one is left alone.
  std::error_code error;
  const bool managed_dir = std::filesystem::is_regular_file(user_service_file(dir, label), error) &&
                           std::filesystem::weakly_canonical(dir, error) ==
                               std::filesystem::weakly_canonical(default_systemd_user_dir(), error);
  // A unit that cannot be stopped is still running: report it, but still
  // remove the definition so it does not come back at the next login.
  const bool stopped = !managed_dir || systemctl_user_disable(dir, label);
  const bool removed = remove_systemd_unit(dir, label);
  if (managed_dir) {
    (void)systemctl_user_reload();
  }
  return stopped && removed;
#else
  (void)launchctl_bootout(label);
  std::error_code error;
  std::filesystem::remove(user_service_file(dir, label), error);
  return !error;
#endif
}

}  // namespace

LaunchAgentSpec supervisor_spec(const SupervisorConfig& config) {
  LaunchAgentSpec spec;
  spec.label = kSupervisorLabel;
  spec.program_args = {config.cgraph_binary.string(), "daemon", "sync"};
  // The scheduled sync runs without the install command's flags, so the roots
  // given as flags travel in its arguments. Roots from supervisor.json do not:
  // the sync re-reads that file, so later edits to it take effect.
  for (const auto& root : config.flag_search_roots) {
    spec.program_args.insert(spec.program_args.end(), {"--search-root", std::filesystem::absolute(root).string()});
  }
  for (const auto& excluded : config.flag_exclude) {
    spec.program_args.insert(spec.program_args.end(), {"--exclude", std::filesystem::absolute(excluded).string()});
  }
  spec.keep_alive = false;                              // one-shot reconcile, not a resident proc
  spec.run_at_load = true;                              // reconcile at login
  spec.start_interval = config.reconcile_interval_seconds;  // and periodically after
  return spec;
}

std::vector<std::string> supervisor_uninstall_labels(const SupervisorConfig& config) {
  // The supervisor goes first: a scheduled sync running mid-uninstall would
  // otherwise re-add the per-repo daemons just removed.
  std::vector<std::string> labels{kSupervisorLabel};
  for (const auto& hash : installed_managed_hashes(config.launch_agents_dir)) {
    labels.push_back(per_repo_agent_label(hash));
  }
  return labels;
}

std::filesystem::path default_user_service_dir() {
#if defined(__linux__)
  return default_systemd_user_dir();
#else
  return default_launch_agents_dir();
#endif
}

std::filesystem::path user_service_file(const std::filesystem::path& dir, const std::string& label) {
  return dir / (label + kUserServiceExtension);
}

std::filesystem::path write_user_service(const std::filesystem::path& dir, const LaunchAgentSpec& spec) {
#if defined(__linux__)
  return write_systemd_unit(dir, spec);
#else
  return write_launch_agent(dir, spec);
#endif
}

bool mcp_json_registers_cgraph(const std::filesystem::path& mcp_json) {
  std::ifstream input(mcp_json);
  if (!input) {
    return false;
  }
  try {
    const auto json = nlohmann::json::parse(input);
    const auto servers = json.find("mcpServers");
    if (servers == json.end() || !servers->is_object()) {
      return false;
    }
    return servers->contains("cgraph");
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

std::vector<TrackedRepo> discover_tracked_repos(const SupervisorConfig& config) {
  std::set<std::filesystem::path> excluded;
  for (const auto& entry : config.exclude) {
    excluded.insert(canonical_project_root(entry));
  }

  // Keyed by root_hash: dedups repos reachable via multiple search roots and
  // yields a deterministic order (std::map iterates sorted by key).
  std::map<std::string, TrackedRepo> by_hash;
  const auto consider = [&](const std::filesystem::path& dir) {
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
      return;
    }
    const auto mcp = dir / ".mcp.json";
    if (!std::filesystem::exists(mcp, error) || !mcp_json_registers_cgraph(mcp)) {
      return;
    }
    const auto canonical = canonical_project_root(dir);
    if (excluded.count(canonical) != 0) {
      return;
    }
    const auto hash = stable_project_hash(canonical);
    by_hash[hash] = TrackedRepo{canonical, hash};
  };

  for (const auto& search_root : config.search_roots) {
    consider(search_root);  // the search root may itself be a repo
    std::error_code error;
    std::filesystem::directory_iterator it(search_root, error);
    if (error) {
      continue;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
      if (error) {
        break;
      }
      consider(it->path());
    }
  }

  std::vector<TrackedRepo> out;
  out.reserve(by_hash.size());
  for (auto& [hash, repo] : by_hash) {
    out.push_back(repo);
  }
  return out;
}

ReconcilePlan reconcile_tracked_repos(
    const std::vector<TrackedRepo>& discovered,
    const std::vector<std::string>& installed_hashes) {
  const std::set<std::string> installed(installed_hashes.begin(), installed_hashes.end());
  std::set<std::string> discovered_hashes;
  ReconcilePlan plan;
  for (const auto& repo : discovered) {
    discovered_hashes.insert(repo.root_hash);
    if (installed.count(repo.root_hash) == 0) {
      plan.to_add.push_back(repo);
    }
  }
  for (const auto& hash : installed_hashes) {
    if (discovered_hashes.count(hash) == 0) {
      plan.to_remove.push_back(hash);
    }
  }
  return plan;
}

std::vector<std::string> installed_managed_hashes(const std::filesystem::path& launch_agents_dir) {
  std::vector<std::string> out;
  std::error_code error;
  std::filesystem::directory_iterator it(launch_agents_dir, error);
  if (error) {
    return out;
  }
  for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
    if (error) {
      break;
    }
    const auto& path = it->path();
    if (path.extension() != kUserServiceExtension) {
      continue;
    }
    const auto hash = root_hash_from_label(path.stem().string());
    if (!hash.empty()) {
      out.push_back(hash);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

DrainerResidencyAction decide_drainer_residency(bool plist_exists, bool service_loaded) {
  // Installation stays explicit. Reconcile only restores residency of a drainer
  // that was installed (plist present) but has since fallen out of the domain.
  if (plist_exists && !service_loaded) {
    return DrainerResidencyAction::kRebootstrap;
  }
  return DrainerResidencyAction::kNone;
}

SupervisorSyncResult supervisor_sync(const SupervisorConfig& config, bool apply) {
  const auto discovered = discover_tracked_repos(config);
  const auto installed = installed_managed_hashes(config.launch_agents_dir);
  SupervisorSyncResult result;
  result.plan = reconcile_tracked_repos(discovered, installed);
  if (!apply) {
    return result;
  }
  for (const auto& repo : result.plan.to_add) {
    const auto spec = per_repo_spec(config, repo);
    const auto written = write_user_service(config.launch_agents_dir, spec);
    if (written.empty() || !load_user_service(config.launch_agents_dir, spec.label)) {
      result.failed.push_back(spec.label);
    }
  }
  for (const auto& hash : result.plan.to_remove) {
    // Best-effort stop; removing the definition is what matters.
    (void)unload_and_remove_user_service(config.launch_agents_dir, per_repo_agent_label(hash));
  }

  // Restore residency of an installed-but-unloaded enrichment drainer. The
  // drainer is installed explicitly (`cgraph drain install`); nothing else
  // reconciled it, so a booted-out or login-lost service stayed silently dormant.
  const std::string drainer_label(kDrainerLabel);
  const auto drainer_plist = config.launch_agents_dir / (drainer_label + ".plist");
  std::error_code plist_ec;
  const bool drainer_plist_exists = std::filesystem::is_regular_file(drainer_plist, plist_ec);
  if (decide_drainer_residency(drainer_plist_exists, launchctl_is_loaded(drainer_label)) ==
      DrainerResidencyAction::kRebootstrap) {
    if (launchctl_bootstrap(drainer_plist)) {
      result.drainer_rebootstrapped = true;
    } else {
      result.failed.push_back(drainer_label);
    }
  }
  return result;
}

bool supervisor_install(const SupervisorConfig& config) {
  const auto spec = supervisor_spec(config);
  if (write_user_service(config.launch_agents_dir, spec).empty() ||
      !load_user_service(config.launch_agents_dir, spec.label)) {
    return false;
  }
  return supervisor_sync(config, /*apply=*/true).failed.empty();
}

bool supervisor_uninstall(const SupervisorConfig& config) {
  bool ok = unload_and_remove_user_service(config.launch_agents_dir, kSupervisorLabel);
  // Listed only once the supervisor is gone, so a daemon its last sync added is
  // caught too; the supervisor entry itself is already handled.
  for (const auto& label : supervisor_uninstall_labels(config)) {
    if (label != kSupervisorLabel) {
      ok = unload_and_remove_user_service(config.launch_agents_dir, label) && ok;
    }
  }
  return ok;
}

std::vector<SupervisorRepoStatus> supervisor_status(const SupervisorConfig& config) {
  std::vector<SupervisorRepoStatus> out;
  for (auto& repo : discover_tracked_repos(config)) {
    const auto identity = daemon_identity_for(repo.root);
    const bool live = unix_endpoint_is_live(unix_socket_path(identity));
    out.push_back(SupervisorRepoStatus{std::move(repo), live});
  }
  return out;
}

}  // namespace cgraph
