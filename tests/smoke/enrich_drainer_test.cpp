#include "cgraph/enrich_drainer.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
  std::ofstream(path, std::ios::binary) << contents;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// Exercises the drainer's portable surface against a real temp tree: script
// resolution walks up to integrations/always-on, the spec renders the expected
// launchd keys (/bin/sh + absolute binaries + chunk cap, RunAtLoad, 4-hour
// StartInterval, no KeepAlive), and the plist write/installed/remove layer round
// trips. Real launchctl bootstrap/bootout is the OS seam proven manually.
int main() {
  bool ok = true;

  const auto base =
      fs::weakly_canonical(fs::temp_directory_path()) / "cgraph_enrich_drainer_test";
  fs::remove_all(base);

  // Script resolution mirrors the skills-source walk-up.
  const auto repo = base / "repo";
  const auto script = repo / "integrations" / "always-on" / "cgraph-enrich-drain.sh";
  write_file(script, "#!/bin/sh\n");
  const auto exe = repo / "build" / "default" / "src" / "cli" / "cgraph";
  write_file(exe, "");
  expect(ok, cgraph::resolve_drain_script(exe, base) == script,
         "resolve_drain_script finds the script from the executable path");
  expect(ok, cgraph::resolve_drain_script(base / "nowhere" / "cgraph", repo / "src") == script,
         "resolve_drain_script falls back to walking up from cwd");
  expect(ok, cgraph::resolve_drain_script(base / "nowhere" / "cgraph", base).empty(),
         "resolve_drain_script returns empty when no ancestor has the script");

  cgraph::DrainerConfig config;
  config.script_path = script;
  config.cgraph_binary = exe;
  config.client_binary = repo / "build" / "default" / "src" / "client" / "cgraph-client";
  config.launch_agents_dir = base / "LaunchAgents";
  config.chunk_cap = 7;

  // Spec: host CLI dispatch stays in the script; the plist only names /bin/sh,
  // the script, and the absolute binaries it needs.
  const auto spec = cgraph::drainer_spec(config);
  expect(ok, spec.label == cgraph::kDrainerLabel, "spec carries the drainer label");
  expect(ok, spec.program_args.size() == 5 && spec.program_args[0] == "/bin/sh" &&
                 spec.program_args[1] == script.string() &&
                 spec.program_args[2] == exe.string() &&
                 spec.program_args[4] == "7",
         "spec runs /bin/sh <script> <cgraph> <client> <chunk-cap>");
  expect(ok, !spec.keep_alive && spec.run_at_load, "one-shot sweep at load, no KeepAlive");
  expect(ok, spec.start_interval.has_value() && *spec.start_interval == 14400,
         "default cadence is every 4 hours (drains a few-hundred-chunk backlog within days)");

  const auto plist = cgraph::render_launch_agent(spec);
  expect(ok, contains(plist, "com.cgraph.enrich-drain"), "plist names the label");
  expect(ok, contains(plist, "cgraph-enrich-drain.sh"), "plist references the script");
  expect(ok, contains(plist, "StartInterval"), "plist schedules the interval");
  expect(ok, !spec.log_path.empty() && contains(plist, "StandardOutPath") &&
                 contains(plist, "StandardErrorPath") &&
                 contains(plist, "cgraph-enrich-drain.log"),
         "sweep output is captured to a log file");
  expect(ok, !contains(plist, "EnvironmentVariables"),
         "no env dict when the host CLI is unresolved");

  // Host CLI resolved at install time is baked into the agent's environment:
  // launchd's PATH lacks interactive-shell dirs (shims live in .zshrc-managed
  // PATH), so runtime resolution inside the script cannot be the only path.
  cgraph::DrainerConfig with_cli = config;
  with_cli.host_cli = "/Users/example/.agents/.cache/shims/claude";
  const auto cli_spec = cgraph::drainer_spec(with_cli);
  const auto cli_plist = cgraph::render_launch_agent(cli_spec);
  expect(ok, contains(cli_plist, "EnvironmentVariables"),
         "resolved host CLI renders an env dict");
  expect(ok, contains(cli_plist, "CGRAPH_HOST_CLI") &&
                 contains(cli_plist, "/Users/example/.agents/.cache/shims/claude"),
         "env dict carries CGRAPH_HOST_CLI with the resolved path");

  // Plist file layer round-trips without launchctl: write, detect, remove.
  expect(ok, !cgraph::drainer_installed(config), "not installed before write");
  const auto written =
      cgraph::write_launch_agent(config.launch_agents_dir, spec);
  expect(ok, !written.empty() && cgraph::drainer_installed(config),
         "written plist is detected as installed");
  expect(ok, cgraph::drainer_uninstall(config), "uninstall removes the plist");
  expect(ok, !cgraph::drainer_installed(config), "not installed after uninstall");
  expect(ok, cgraph::drainer_uninstall(config), "uninstall is idempotent");

  fs::remove_all(base);
  if (!ok) {
    return 1;
  }
  std::cout << "enrich drainer lifecycle ok\n";
  return 0;
}
