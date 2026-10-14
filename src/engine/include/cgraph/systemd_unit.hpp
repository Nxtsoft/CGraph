#pragma once

#include "cgraph/launch_agent.hpp"

#include <filesystem>
#include <string>

namespace cgraph {

// systemd --user counterpart of the launchd LaunchAgent layer, driven by the
// same LaunchAgentSpec so the supervisor reconciles one model on both systems.
// A spec's label is the unit name (`<label>.service`); launchd labels such as
// com.cgraph.graphd.<hash> are valid systemd unit names unchanged.
//
// Mapping: keep_alive -> Restart=always; run_at_load -> WantedBy=default.target
// (started at login); start_interval -> a `<label>.timer` (OnActiveSec=0,
// OnUnitActiveSec=<interval>) driving a Type=oneshot service, which is what is
// enabled; working_directory, log_path (append:) and env carry over.

// The `.service` unit text. Pure string function.
[[nodiscard]] std::string render_systemd_service(const LaunchAgentSpec& spec);

// The `.timer` unit text; only meaningful when spec.start_interval is set.
[[nodiscard]] std::string render_systemd_timer(const LaunchAgentSpec& spec);

// $XDG_CONFIG_HOME/systemd/user, else ~/.config/systemd/user. Empty if neither
// XDG_CONFIG_HOME nor HOME is set.
[[nodiscard]] std::filesystem::path default_systemd_user_dir();

// Write <label>.service (and <label>.timer when start_interval is set) into
// unit_dir. Returns the .service path, or empty on failure.
[[nodiscard]] std::filesystem::path write_systemd_unit(const std::filesystem::path& unit_dir,
                                                       const LaunchAgentSpec& spec);

// Remove <label>.service and <label>.timer from unit_dir. False if a file that
// exists could not be removed.
[[nodiscard]] bool remove_systemd_unit(const std::filesystem::path& unit_dir, const std::string& label);

// systemctl --user glue (Linux). enable reloads the manager and enables + starts
// the timer when unit_dir holds one for the label, else the service. disable
// stops + disables whichever was enabled and stops the timer's service. Both
// return false on non-Linux or on error.
[[nodiscard]] bool systemctl_user_enable(const std::filesystem::path& unit_dir, const std::string& label);
[[nodiscard]] bool systemctl_user_disable(const std::filesystem::path& unit_dir, const std::string& label);

// `systemctl --user daemon-reload`, so the manager forgets removed unit files.
[[nodiscard]] bool systemctl_user_reload();

// True when `<label>.service` is active. False on non-Linux.
[[nodiscard]] bool systemctl_user_is_active(const std::string& label);

}  // namespace cgraph
