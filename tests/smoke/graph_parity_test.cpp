#include "cgraph/analysis.hpp"
#include "cgraph/dedup.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/file_extraction.hpp"
#include "cgraph/graph_builder.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

bool has_label(const cgraph::GraphSnapshot& graph, std::string_view label_fragment) {
  for (const auto& node : graph.nodes) {
    if (node.label.find(label_fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_graph_parity_test";
  std::filesystem::remove_all(root);

  write_file(root / "src" / "main.py", R"py(
import os

class PaymentService:
    def run(self):
        return helper()

def helper():
    return os.getcwd()
)py");

  write_file(root / "src" / "service.ts", R"ts(
export class Worker {
  run() {
    return build();
  }
}

function build() {
  return "ok";
}
)ts");

  const auto detected = cgraph::detect_project_files(root);
  if (detected.size() != 2) {
    std::filesystem::remove_all(root);
    return 1;
  }

  std::vector<cgraph::Fragment> fragments;
  std::vector<cgraph::RawCall> raw_calls;
  for (const auto& file : detected) {
    auto result = cgraph::extract_detected_file(file, std::filesystem::weakly_canonical(root));
    fragments.push_back(std::move(result.fragment));
    raw_calls.insert(raw_calls.end(), result.raw_calls.begin(), result.raw_calls.end());
  }

  auto graph = cgraph::merge_fragments(fragments);
  cgraph::resolve_raw_calls(graph, raw_calls);
  cgraph::semantic_dedup(graph);
  const auto community_result = cgraph::detect_communities(graph);
  if (community_result.cluster_count < 1) {
    std::filesystem::remove_all(root);
    return 1;
  }
  cgraph::analyze_graph(graph);

  std::filesystem::remove_all(root);

  if (!has_label(graph, "PaymentService") || !has_label(graph, "Worker")) {
    return 1;
  }
  if (graph.edges.empty()) {
    return 1;
  }
  for (const auto& node : graph.nodes) {
    if (!node.properties.contains("community") || !node.properties.contains("degree_centrality")) {
      return 1;
    }
  }

  return 0;
}
