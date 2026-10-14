#include "cgraph/launch_agent.hpp"
#include "cgraph/systemd_unit.hpp"

#include <cstdlib>
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

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

// Renders the units the supervisor installs on Linux and round-trips them
// through a temp directory. Where systemd-analyze exists, systemd itself parses
// each unit and the test fails on any complaint, so quoting mistakes surface as
// systemd sees them rather than as this file imagines them.
int main() {
  bool ok = true;
  const auto base = fs::temp_directory_path() / "cgraph_systemd_unit_test";
  fs::remove_all(base);
  const auto unit_dir = base / "user";

  // A per-repo daemon whose root has a space, a % and a $ in it: each must reach
  // graphd as one literal argument.
  const auto bin_dir = base / "bin dir";
  fs::create_directories(bin_dir);
  const auto graphd = bin_dir / "graphd";
  std::ofstream(graphd) << "#!/bin/sh\nexit 0\n";
  fs::permissions(graphd, fs::perms::owner_all);
  cgraph::LaunchAgentSpec daemon;
  daemon.label = cgraph::per_repo_agent_label("abc123");
  daemon.program_args = {graphd.string(), "--root", "/work/my repo 100%$HOME", "--idle-timeout", "0"};
  daemon.keep_alive = true;
  daemon.run_at_load = true;
  daemon.env = {{"CGRAPH_NOTE", "say \"hi\" 5%"}};
  const auto service = cgraph::render_systemd_service(daemon);
  expect(ok, contains(service, "ExecStart=\"" + graphd.string() + "\" \"--root\" \"/work/my repo 100%%$$HOME\""),
         "ExecStart quotes each argument and escapes % and $");
  expect(ok, contains(service, "Type=simple\n"), "a resident daemon is Type=simple");
  expect(ok, contains(service, "Restart=always\n"), "keep_alive restarts whatever the exit");
  expect(ok, contains(service, "StartLimitIntervalSec=0\n"), "keep_alive never hits the start limit");
  expect(ok, contains(service, "WantedBy=default.target\n"), "run_at_load starts at login");
  expect(ok, contains(service, "Environment=\"CGRAPH_NOTE=say \\\"hi\\\" 5%%\"\n"), "env values are quoted");

  // The supervisor: a oneshot service driven by a timer; the timer is what is
  // enabled, so the service has no [Install] of its own.
  cgraph::LaunchAgentSpec supervisor;
  supervisor.label = cgraph::kSupervisorLabel;
  supervisor.program_args = {"/bin/true", "daemon", "sync"};
  supervisor.run_at_load = true;
  supervisor.start_interval = 300;
  const auto sup_service = cgraph::render_systemd_service(supervisor);
  const auto sup_timer = cgraph::render_systemd_timer(supervisor);
  expect(ok, contains(sup_service, "Type=oneshot\n"), "the periodic supervisor is oneshot");
  expect(ok, !contains(sup_service, "[Install]"), "the timer, not the service, is enabled");
  expect(ok, !contains(sup_service, "Restart="), "a oneshot is not restarted");
  expect(ok, contains(sup_timer, "OnActiveSec=0\n"), "run_at_load fires when the timer starts");
  expect(ok, contains(sup_timer, "OnUnitActiveSec=300s\n"), "start_interval repeats the run");
  expect(ok, contains(sup_timer, "Unit=com.cgraph.supervisor.service\n"), "the timer drives the service");
  expect(ok, contains(sup_timer, "WantedBy=timers.target\n"), "the timer starts at login");

  // Write and remove round-trip: a daemon writes one file, the supervisor two.
  const auto daemon_path = cgraph::write_systemd_unit(unit_dir, daemon);
  expect(ok, daemon_path == unit_dir / "com.cgraph.graphd.abc123.service", "daemon unit path is <label>.service");
  expect(ok, read_file(daemon_path) == service, "written daemon unit is the rendered text");
  expect(ok, !fs::exists(unit_dir / "com.cgraph.graphd.abc123.timer"), "no timer without start_interval");
  const auto sup_path = cgraph::write_systemd_unit(unit_dir, supervisor);
  expect(ok, !sup_path.empty() && fs::exists(unit_dir / "com.cgraph.supervisor.timer"),
         "supervisor writes service and timer");

  // systemd's own parser, where present (Linux hosts and CI).
  if (std::system("command -v systemd-analyze >/dev/null 2>&1") == 0) {
    for (const auto& unit : {daemon_path, sup_path, unit_dir / "com.cgraph.supervisor.timer"}) {
      const auto log = base / "verify.log";
      const std::string cmd = "systemd-analyze verify --man=no '" + unit.string() + "' >'" + log.string() + "' 2>&1";
      const int status = std::system(cmd.c_str());
      const auto output = read_file(log);
      if (status != 0 || !output.empty()) {
        std::cerr << "systemd-analyze verify " << unit << ":\n" << output;
      }
      expect(ok, status == 0 && output.empty(), "systemd parses the unit without complaint");
    }
  }

  expect(ok, cgraph::remove_systemd_unit(unit_dir, supervisor.label), "remove reports success");
  expect(ok, !fs::exists(sup_path) && !fs::exists(unit_dir / "com.cgraph.supervisor.timer"),
         "remove deletes service and timer");
  expect(ok, cgraph::remove_systemd_unit(unit_dir, daemon.label) && !fs::exists(daemon_path),
         "remove deletes a timer-less unit");
  expect(ok, cgraph::remove_systemd_unit(unit_dir, daemon.label), "removing an absent unit is not an error");

  fs::remove_all(base);
  return ok ? 0 : 1;
}
