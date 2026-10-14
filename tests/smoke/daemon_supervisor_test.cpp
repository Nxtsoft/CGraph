#include "cgraph/daemon_identity.hpp"
#include "cgraph/daemon_supervisor.hpp"
#include "cgraph/launch_agent.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

void write_file(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << contents;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Exercises the supervisor's portable logic against a real temp tree: discovery
// selects exactly the cgraph-registered repos, reconcile computes the right diff
// and is idempotent, the plist renderer emits the expected launchd keys, and the
// plist file layer (write / installed_managed_hashes / uninstall removal) round
// trips. Real launchctl bootstrap/bootout is the OS seam validated end-to-end
// manually; these tests never load a real service.
int main() {
  bool ok = true;

  const auto base = fs::temp_directory_path() / "cgraph_supervisor_test";
  fs::remove_all(base);
  const auto tree = base / "tree";

  // A cgraph-registered repo, a repo registering a different MCP server, and a
  // plain directory with no .mcp.json.
  write_file(tree / "repo_cgraph" / ".mcp.json",
             R"({"mcpServers":{"cgraph":{"command":"/usr/local/bin/cgraph-mcp"}}})");
  write_file(tree / "repo_other" / ".mcp.json",
             R"({"mcpServers":{"other":{"command":"/usr/local/bin/other-mcp"}}})");
  fs::create_directories(tree / "repo_none");
  write_file(tree / "loose_file.txt", "not a repo\n");

  cgraph::SupervisorConfig config;
  config.search_roots = {tree};
  config.launch_agents_dir = base / "LaunchAgents";
  config.graphd_binary = "/opt/cgraph/graphd";
  config.cgraph_binary = "/opt/cgraph/cgraph";

  // --- Discovery: exactly the cgraph repo, keyed by the daemon identity hash. ---
  const auto discovered = cgraph::discover_tracked_repos(config);
  expect(ok, discovered.size() == 1, "discovery finds exactly the cgraph-registered repo");
  if (discovered.size() == 1) {
    const auto& repo = discovered.front();
    const auto identity = cgraph::daemon_identity_for(tree / "repo_cgraph");
    expect(ok, repo.root_hash == identity.root_hash,
           "discovery key equals the daemon endpoint identity hash");
    expect(ok, repo.root == cgraph::canonical_project_root(tree / "repo_cgraph"),
           "discovered root is the canonical project root");
  }

  expect(ok, cgraph::mcp_json_registers_cgraph(tree / "repo_cgraph" / ".mcp.json"),
         "mcp_json_registers_cgraph true for a cgraph server");
  expect(ok, !cgraph::mcp_json_registers_cgraph(tree / "repo_other" / ".mcp.json"),
         "mcp_json_registers_cgraph false for a non-cgraph server");
  expect(ok, !cgraph::mcp_json_registers_cgraph(tree / "repo_none" / ".mcp.json"),
         "mcp_json_registers_cgraph false when absent");

  // --- Reconcile: add missing, remove orphaned, idempotent at steady state. ---
  const std::string hash = discovered.empty() ? "" : discovered.front().root_hash;
  const auto add_plan = cgraph::reconcile_tracked_repos(discovered, {});
  expect(ok, add_plan.to_add.size() == 1 && add_plan.to_remove.empty(),
         "reconcile schedules the discovered repo for add");
  const auto steady = cgraph::reconcile_tracked_repos(discovered, {hash});
  expect(ok, steady.to_add.empty() && steady.to_remove.empty(),
         "reconcile is idempotent when installed == discovered");
  const auto orphan = cgraph::reconcile_tracked_repos(discovered, {hash, "deadbeef"});
  expect(ok, orphan.to_add.empty() && orphan.to_remove.size() == 1 && orphan.to_remove.front() == "deadbeef",
         "reconcile schedules an orphaned installed hash for removal");

  // --- Drainer residency decision: pure, launchctl-free. Installation stays
  // explicit; reconcile only re-bootstraps an installed-but-unloaded drainer. ---
  expect(ok, cgraph::decide_drainer_residency(/*plist_exists=*/true, /*service_loaded=*/false) ==
                 cgraph::DrainerResidencyAction::kRebootstrap,
         "installed drainer whose service is unloaded is re-bootstrapped");
  expect(ok, cgraph::decide_drainer_residency(/*plist_exists=*/true, /*service_loaded=*/true) ==
                 cgraph::DrainerResidencyAction::kNone,
         "installed and already-loaded drainer is left untouched");
  expect(ok, cgraph::decide_drainer_residency(/*plist_exists=*/false, /*service_loaded=*/false) ==
                 cgraph::DrainerResidencyAction::kNone,
         "absent drainer plist is never installed by reconcile");
  expect(ok, cgraph::decide_drainer_residency(/*plist_exists=*/false, /*service_loaded=*/true) ==
                 cgraph::DrainerResidencyAction::kNone,
         "no plist means reconcile takes no residency action");

  // --- Plist rendering: per-repo daemon and supervisor. ---
  cgraph::LaunchAgentSpec repo_spec;
  repo_spec.label = cgraph::per_repo_agent_label("abc123");
  repo_spec.program_args = {"/opt/cgraph/graphd", "--root", "/x/y", "--idle-timeout", "0"};
  repo_spec.keep_alive = true;
  repo_spec.run_at_load = true;
  const auto repo_plist = cgraph::render_launch_agent(repo_spec);
  expect(ok, contains(repo_plist, "<string>com.cgraph.graphd.abc123</string>"), "plist has per-repo label");
  expect(ok, contains(repo_plist, "<string>--idle-timeout</string>") && contains(repo_plist, "<string>0</string>"),
         "plist wires never-idle graphd args");
  expect(ok, contains(repo_plist, "<key>KeepAlive</key>\n  <true/>"), "plist enables KeepAlive");
  expect(ok, contains(repo_plist, "<key>RunAtLoad</key>\n  <true/>"), "plist enables RunAtLoad");

  cgraph::LaunchAgentSpec sup_spec;
  sup_spec.label = cgraph::kSupervisorLabel;
  sup_spec.program_args = {"/opt/cgraph/cgraph", "daemon", "sync"};
  sup_spec.run_at_load = true;
  sup_spec.start_interval = 300;
  const auto sup_plist = cgraph::render_launch_agent(sup_spec);
  expect(ok, contains(sup_plist, "<key>StartInterval</key>\n  <integer>300</integer>"),
         "supervisor plist sets StartInterval");
  expect(ok, contains(sup_plist, "<string>sync</string>"), "supervisor plist runs daemon sync");

  // --- Plist file layer: write, discover installed, uninstall removes. ---
  const auto written = cgraph::write_user_service(config.launch_agents_dir, repo_spec);
  expect(ok, !written.empty() && fs::exists(written), "write_user_service writes the definition");
  expect(ok, written == cgraph::user_service_file(config.launch_agents_dir, repo_spec.label),
         "the definition lands where user_service_file says");
  const auto installed = cgraph::installed_managed_hashes(config.launch_agents_dir);
  expect(ok, std::find(installed.begin(), installed.end(), "abc123") != installed.end(),
         "installed_managed_hashes reports the written per-repo agent");
  expect(ok, cgraph::root_hash_from_label("com.cgraph.graphd.abc123") == "abc123",
         "root_hash_from_label parses a managed label");
  expect(ok, cgraph::root_hash_from_label(cgraph::kSupervisorLabel).empty(),
         "root_hash_from_label ignores the supervisor label");

  // sync with apply=false computes the plan without any launchctl side effect.
  const auto dry = cgraph::supervisor_sync(config, /*apply=*/false);
  expect(ok, dry.plan.to_add.size() == 1 && dry.plan.to_remove.size() == 1,
         "dry sync adds the discovered repo and removes the orphaned abc123 plist");

  // --- Supervisor spec: only flag roots travel into the scheduled sync. Roots
  // from supervisor.json must not be frozen there, or editing the file later
  // would never drop a root. ---
  cgraph::SupervisorConfig flagged = config;
  const auto file_root = base / "from-supervisor-json";
  const auto flag_root = tree;
  const auto file_exclude = base / "file-excluded";
  const auto flag_exclude = tree / "repo_other";
  flagged.search_roots = {file_root, flag_root};
  flagged.flag_search_roots = {flag_root};
  flagged.exclude = {file_exclude, flag_exclude};
  flagged.flag_exclude = {flag_exclude};
  const auto sync_args = cgraph::supervisor_spec(flagged).program_args;
  const auto has_pair = [&](const std::string& flag, const fs::path& value) {
    for (std::size_t i = 0; i + 1 < sync_args.size(); ++i) {
      if (sync_args[i] == flag && sync_args[i + 1] == fs::absolute(value).string()) return true;
    }
    return false;
  };
  expect(ok, sync_args.size() >= 3 && sync_args[1] == "daemon" && sync_args[2] == "sync",
         "supervisor runs `daemon sync`");
  expect(ok, has_pair("--search-root", flag_root), "a --search-root flag reaches the scheduled sync");
  expect(ok, has_pair("--exclude", flag_exclude), "an --exclude flag reaches the scheduled sync");
  expect(ok, !has_pair("--search-root", file_root), "a supervisor.json root is not frozen into the sync");
  expect(ok, !has_pair("--exclude", file_exclude), "a supervisor.json exclude is not frozen into the sync");
  expect(ok, sync_args.size() == 7, "exactly one flag root and one flag exclude are forwarded");

  // --- Uninstall order: supervisor first, then every managed per-repo daemon. ---
  (void)cgraph::write_user_service(config.launch_agents_dir, sup_spec);
  const auto order = cgraph::supervisor_uninstall_labels(config);
  expect(ok, !order.empty() && order.front() == cgraph::kSupervisorLabel,
         "uninstall stops the supervisor before any per-repo daemon");
  expect(ok, std::find(order.begin() + 1, order.end(), repo_spec.label) != order.end(),
         "uninstall then removes the installed per-repo daemon");

  // uninstall removes all managed plists (launchctl bootout of never-loaded labels
  // is a harmless no-op; the file removal is the observable residue guarantee).
  const bool uninstalled = cgraph::supervisor_uninstall(config);
  expect(ok, uninstalled, "uninstall reports success");
  expect(ok, cgraph::installed_managed_hashes(config.launch_agents_dir).empty(),
         "uninstall leaves no managed per-repo plist");
  expect(ok, !fs::exists(cgraph::user_service_file(config.launch_agents_dir, cgraph::kSupervisorLabel)),
         "uninstall removes the supervisor definition");
  expect(ok, !fs::exists(config.launch_agents_dir / (std::string(cgraph::kSupervisorLabel) + ".timer")),
         "uninstall removes the supervisor timer (Linux) and leaves nothing behind");

  fs::remove_all(base);
  return ok ? 0 : 1;
}
