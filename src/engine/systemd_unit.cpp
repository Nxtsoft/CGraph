#include "cgraph/systemd_unit.hpp"

#include "cgraph/atomic_write.hpp"

#include <cstdlib>
#include <sstream>
#include <system_error>

namespace cgraph {
namespace {

// systemd expands %-specifiers in every value written here, so a literal % is %%.
std::string escape_specifiers(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    out += ch;
    if (ch == '%') {
      out += '%';
    }
  }
  return out;
}

// One ExecStart argument: double-quoted with C escapes so spaces stay inside the
// word, %% for specifiers and $$ so the manager does not substitute variables.
std::string quote_exec_arg(const std::string& value) {
  std::string out = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '%': out += "%%"; break;
      case '$': out += "$$"; break;
      default: out += ch; break;
    }
  }
  out += '"';
  return out;
}

std::string quote_environment(const std::string& key, const std::string& value) {
  std::string out = "\"";
  for (const char ch : key + "=" + value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '%': out += "%%"; break;
      default: out += ch; break;
    }
  }
  out += '"';
  return out;
}

std::filesystem::path timer_path(const std::filesystem::path& unit_dir, const std::string& label) {
  return unit_dir / (label + ".timer");
}

std::filesystem::path service_path(const std::filesystem::path& unit_dir, const std::string& label) {
  return unit_dir / (label + ".service");
}

}  // namespace

std::string render_systemd_service(const LaunchAgentSpec& spec) {
  std::ostringstream out;
  out << "# Managed by cgraph; `cgraph daemon uninstall` removes it.\n"
      << "[Unit]\n"
      << "Description=cgraph " << escape_specifiers(spec.label) << "\n";
  if (spec.keep_alive) {
    // launchd KeepAlive semantics: relaunch whatever the exit, throttled, never
    // given up on. graphd exits 0 when a client-spawned daemon already serves the
    // root, so a start limit would park the unit failed for good.
    out << "StartLimitIntervalSec=0\n";
  }
  out << "\n"
      << "[Service]\n"
      << "Type=" << (spec.start_interval ? "oneshot" : "simple") << "\n"
      << "ExecStart=";
  for (std::size_t index = 0; index < spec.program_args.size(); ++index) {
    out << (index == 0 ? "" : " ") << quote_exec_arg(spec.program_args[index]);
  }
  out << "\n";
  if (spec.keep_alive) {
    out << "Restart=always\n"
        << "RestartSec=10\n";
  }
  if (!spec.working_directory.empty()) {
    out << "WorkingDirectory=" << escape_specifiers(spec.working_directory.string()) << "\n";
  }
  if (!spec.log_path.empty()) {
    const auto log = escape_specifiers(spec.log_path.string());
    out << "StandardOutput=append:" << log << "\n"
        << "StandardError=append:" << log << "\n";
  }
  for (const auto& [key, value] : spec.env) {
    out << "Environment=" << quote_environment(key, value) << "\n";
  }
  if (spec.run_at_load && !spec.start_interval) {
    out << "\n[Install]\n"
        << "WantedBy=default.target\n";
  }
  return out.str();
}

std::string render_systemd_timer(const LaunchAgentSpec& spec) {
  std::ostringstream out;
  out << "# Managed by cgraph; `cgraph daemon uninstall` removes it.\n"
      << "[Unit]\n"
      << "Description=cgraph " << escape_specifiers(spec.label) << " schedule\n\n"
      << "[Timer]\n";
  if (spec.run_at_load) {
    out << "OnActiveSec=0\n";
  }
  out << "OnUnitActiveSec=" << spec.start_interval.value_or(0) << "s\n"
      << "Unit=" << escape_specifiers(spec.label) << ".service\n\n"
      << "[Install]\n"
      << "WantedBy=timers.target\n";
  return out.str();
}

std::filesystem::path default_systemd_user_dir() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "systemd" / "user";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  return std::filesystem::path(home) / ".config" / "systemd" / "user";
}

std::filesystem::path write_systemd_unit(const std::filesystem::path& unit_dir, const LaunchAgentSpec& spec) {
  std::error_code error;
  std::filesystem::create_directories(unit_dir, error);
  if (error) {
    return {};
  }
  if (spec.start_interval &&
      !try_write_file_atomically(timer_path(unit_dir, spec.label), render_systemd_timer(spec)).empty()) {
    return {};
  }
  const auto path = service_path(unit_dir, spec.label);
  return try_write_file_atomically(path, render_systemd_service(spec)).empty() ? path : std::filesystem::path{};
}

bool remove_systemd_unit(const std::filesystem::path& unit_dir, const std::string& label) {
  bool ok = true;
  for (const auto& path : {timer_path(unit_dir, label), service_path(unit_dir, label)}) {
    std::error_code error;
    std::filesystem::remove(path, error);
    ok = ok && !error;
  }
  return ok;
}

bool systemctl_user_reload() {
#if defined(__linux__)
  return run_service_command({"systemctl", "--user", "daemon-reload"}) == 0;
#else
  return false;
#endif
}

bool systemctl_user_enable(const std::filesystem::path& unit_dir, const std::string& label) {
#if defined(__linux__)
  std::error_code error;
  const bool timed = std::filesystem::is_regular_file(timer_path(unit_dir, label), error);
  return systemctl_user_reload() &&
         run_service_command({"systemctl", "--user", "enable", "--now", label + (timed ? ".timer" : ".service")}) == 0;
#else
  (void)unit_dir;
  (void)label;
  return false;
#endif
}

bool systemctl_user_disable(const std::filesystem::path& unit_dir, const std::string& label) {
#if defined(__linux__)
  std::error_code error;
  const bool timed = std::filesystem::is_regular_file(timer_path(unit_dir, label), error);
  if (timed) {
    // Stop before disabling: disable reloads the manager, which re-arms a still
    // running OnActiveSec=0 timer and starts one more sync mid-uninstall. The
    // service has no [Install]; the timer is what was enabled.
    return run_service_command({"systemctl", "--user", "stop", label + ".timer", label + ".service"}) == 0 &&
           run_service_command({"systemctl", "--user", "disable", label + ".timer"}) == 0;
  }
  return run_service_command({"systemctl", "--user", "disable", "--now", label + ".service"}) == 0;
#else
  (void)unit_dir;
  (void)label;
  return false;
#endif
}

}  // namespace cgraph
