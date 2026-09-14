#pragma once

#include "cgraph/operation_stats.hpp"
#include "cgraph/types.hpp"

#include <filesystem>

namespace cgraph {

struct PipelineResult {
  GraphSnapshot graph;
  std::size_t file_count = 0;
  std::vector<std::string> warnings;
  BuildStats stats;
};

[[nodiscard]] PipelineResult run_one_shot(const std::filesystem::path& root);
// Writes graph.json and every derived export, including the module dependency
// diagram (modules.mmd / modules.svg, whole project, no budget). Module names
// are relative to `project_root`.
void write_exports(const GraphSnapshot& graph, const std::filesystem::path& output_dir,
                   const std::filesystem::path& project_root);

}  // namespace cgraph
