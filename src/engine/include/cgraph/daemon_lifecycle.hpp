#pragma once

#include "cgraph/daemon_ops.hpp"

#include <chrono>
#include <filesystem>

namespace cgraph {

using DaemonClock = std::chrono::steady_clock;

struct DaemonLifecycleConfig {
  std::filesystem::path endpoint_path;
  std::filesystem::path graph_path;
  std::chrono::seconds idle_timeout{300};
  std::chrono::seconds persist_interval{30};
};

struct DaemonLifecycleState {
  DaemonClock::time_point last_activity{};
  DaemonClock::time_point last_persist{};
  DaemonClock::time_point dirty_since{};
  bool graph_dirty = false;
};

void record_daemon_activity(DaemonLifecycleState& lifecycle, DaemonClock::time_point now);
void mark_graph_dirty(DaemonLifecycleState& lifecycle, DaemonClock::time_point now);
[[nodiscard]] bool should_shutdown_for_idle(
    const DaemonLifecycleState& lifecycle,
    const DaemonLifecycleConfig& config,
    DaemonClock::time_point now);
[[nodiscard]] bool cleanup_daemon_endpoint(const std::filesystem::path& endpoint_path);
[[nodiscard]] bool persist_graph_snapshot(
    const GraphSnapshot& snapshot,
    const std::filesystem::path& graph_path);
[[nodiscard]] bool persist_graph_snapshot(const DaemonState& state, const std::filesystem::path& graph_path);
[[nodiscard]] bool load_graph_snapshot(DaemonState& state, const std::filesystem::path& graph_path);
[[nodiscard]] bool persist_if_due(
    const GraphSnapshot& persistence_snapshot,
    DaemonLifecycleState& lifecycle,
    const DaemonLifecycleConfig& config,
    DaemonClock::time_point now);

}  // namespace cgraph
