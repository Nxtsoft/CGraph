#include "cgraph/launch_agent.hpp"

#include "cgraph/atomic_write.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cgraph {
namespace {

std::string xml_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += ch; break;
    }
  }
  return out;
}

#if defined(__APPLE__)
// Run argv to completion without a shell (no quoting pitfalls). Returns the exit
// status, or -1 if the process could not be spawned.
int run_command(const std::vector<std::string>& argv) {
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    raw.push_back(const_cast<char*>(arg.c_str()));
  }
  raw.push_back(nullptr);

  pid_t pid = 0;
  if (posix_spawnp(&pid, raw[0], nullptr, nullptr, raw.data(), environ) != 0) {
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string gui_domain() {
  return "gui/" + std::to_string(::getuid());
}
#endif

}  // namespace

std::string render_launch_agent(const LaunchAgentSpec& spec) {
  std::ostringstream out;
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n"
      << "<dict>\n"
      << "  <key>Label</key>\n"
      << "  <string>" << xml_escape(spec.label) << "</string>\n"
      << "  <key>ProgramArguments</key>\n"
      << "  <array>\n";
  for (const auto& arg : spec.program_args) {
    out << "    <string>" << xml_escape(arg) << "</string>\n";
  }
  out << "  </array>\n"
      << "  <key>RunAtLoad</key>\n"
      << "  " << (spec.run_at_load ? "<true/>" : "<false/>") << "\n"
      << "  <key>KeepAlive</key>\n"
      << "  " << (spec.keep_alive ? "<true/>" : "<false/>") << "\n";
  if (spec.start_interval) {
    out << "  <key>StartInterval</key>\n"
        << "  <integer>" << *spec.start_interval << "</integer>\n";
  }
  if (!spec.working_directory.empty()) {
    out << "  <key>WorkingDirectory</key>\n"
        << "  <string>" << xml_escape(spec.working_directory.string()) << "</string>\n";
  }
  if (!spec.log_path.empty()) {
    out << "  <key>StandardOutPath</key>\n"
        << "  <string>" << xml_escape(spec.log_path.string()) << "</string>\n"
        << "  <key>StandardErrorPath</key>\n"
        << "  <string>" << xml_escape(spec.log_path.string()) << "</string>\n";
  }
  if (!spec.env.empty()) {
    out << "  <key>EnvironmentVariables</key>\n"
        << "  <dict>\n";
    for (const auto& [key, value] : spec.env) {
      out << "    <key>" << xml_escape(key) << "</key>\n"
          << "    <string>" << xml_escape(value) << "</string>\n";
    }
    out << "  </dict>\n";
  }
  out << "</dict>\n"
      << "</plist>\n";
  return out.str();
}

std::string per_repo_agent_label(const std::string& root_hash) {
  return std::string(kPerRepoLabelPrefix) + root_hash;
}

std::string root_hash_from_label(const std::string& label) {
  const std::string prefix(kPerRepoLabelPrefix);
  if (label.size() <= prefix.size() || label.compare(0, prefix.size(), prefix) != 0) {
    return {};
  }
  return label.substr(prefix.size());
}

std::filesystem::path default_launch_agents_dir() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  return std::filesystem::path(home) / "Library" / "LaunchAgents";
}

std::filesystem::path write_launch_agent(
    const std::filesystem::path& launch_agents_dir, const LaunchAgentSpec& spec) {
  std::error_code error;
  std::filesystem::create_directories(launch_agents_dir, error);
  if (error) {
    return {};
  }
  const auto path = launch_agents_dir / (spec.label + ".plist");
  return try_write_file_atomically(path, render_launch_agent(spec)).empty() ? path : std::filesystem::path{};
}

bool launchctl_bootstrap(const std::filesystem::path& plist_path) {
#if defined(__APPLE__)
  // Re-bootstrap cleanly: bootout any prior instance (ignore failure) so a rewrite
  // of the plist takes effect, then bootstrap the new one.
  return run_command({"launchctl", "bootstrap", gui_domain(), plist_path.string()}) == 0;
#else
  (void)plist_path;
  return false;
#endif
}

bool launchctl_bootout(const std::string& label) {
#if defined(__APPLE__)
  return run_command({"launchctl", "bootout", gui_domain() + "/" + label}) == 0;
#else
  (void)label;
  return false;
#endif
}

bool launchctl_is_loaded(const std::string& label) {
#if defined(__APPLE__)
  // `launchctl print gui/<uid>/<label>` exits 0 when the service is registered
  // in the domain and non-zero (with "Could not find service") when it is not.
  return run_command({"launchctl", "print", gui_domain() + "/" + label}) == 0;
#else
  (void)label;
  return false;
#endif
}

}  // namespace cgraph
