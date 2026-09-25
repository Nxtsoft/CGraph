#include "cgraph/operation_stats.hpp"
#include "cgraph/pipeline.hpp"
#include "cgraph/file_cache.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_pipeline_test";
  const auto out = std::filesystem::temp_directory_path() / "cgraph_pipeline_out";
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(out);

  constexpr const char* kPythonSource = "def main():\n    return helper()\n\ndef helper():\n    return 1\n";
  constexpr const char* kGoSource =
      "package main\n\ntype Service struct{}\n\nfunc (s *Service) Run() {\n\thelper()\n}\n\nfunc helper() {}\n";
  write_file(root / "main.py", kPythonSource);
  write_file(root / "service.go", kGoSource);
  // Detected but extractorless: must surface in the unextracted coverage map,
  // not vanish behind a per-file warning.
  write_file(root / "view.blade.php", "<div>{{ $user->name }}</div>\n");

  const auto result = cgraph::run_one_shot(root);
  if (result.file_count != 3 || result.graph.nodes.empty()) {
    std::filesystem::remove_all(root);
    return 1;
  }
  const auto python_hash = result.graph.source_hashes.find(
      std::filesystem::weakly_canonical(root / "main.py").generic_string());
  const auto go_hash = result.graph.source_hashes.find(
      std::filesystem::weakly_canonical(root / "service.go").generic_string());
  if (python_hash == result.graph.source_hashes.end() || python_hash->second != cgraph::sha256_hex(kPythonSource) ||
      go_hash == result.graph.source_hashes.end() || go_hash->second != cgraph::sha256_hex(kGoSource)) {
    std::filesystem::remove_all(root);
    return 8;
  }

  // The Go file produced real symbols (type + functions), not just a file node.
  bool go_symbol_seen = false;
  for (const auto& node : result.graph.nodes) {
    if (node.label == "Service" || node.label == "Run") {
      go_symbol_seen = true;
      break;
    }
  }
  if (!go_symbol_seen) {
    std::filesystem::remove_all(root);
    return 6;
  }

  if (result.stats.unextracted.size() != 1 || result.stats.unextracted.at("php-blade") != 1) {
    std::filesystem::remove_all(root);
    return 7;
  }

  // Layer A: a real build records per-phase timings and counters at the seam.
  const auto& stats = result.stats;
  if (stats.total_ms() <= 0.0 || stats.extract_ms <= 0.0 || stats.merge_ms <= 0.0 ||
      stats.resolve_ms <= 0.0 || stats.dedup_ms <= 0.0 || stats.communities_ms <= 0.0 ||
      stats.analyze_ms <= 0.0) {
    std::filesystem::remove_all(root);
    return 2;  // every phase must have a measured, positive duration
  }
  if (stats.nodes != result.graph.nodes.size() || stats.edges != result.graph.edges.size()) {
    std::filesystem::remove_all(root);
    return 3;  // counters must match the resulting snapshot
  }
  // A cold one-shot build extracts every file and reuses none.
  if (stats.files_total != 3 || stats.files_extracted != 3 || stats.files_cache_hit != 0 ||
      result.graph.cache_hit_rate != 0.0) {
    std::filesystem::remove_all(root);
    return 4;
  }
  // stats.json body is well-formed and omits the saving estimate on a cold build.
  const auto stats_json = cgraph::build_stats_json(stats);
  if (stats_json["node_count"] != result.graph.nodes.size() ||
      stats_json.contains("cache_saved_ms_estimate") ||
      stats_json["unextracted"]["php-blade"] != 1) {
    std::filesystem::remove_all(root);
    return 5;
  }

  cgraph::write_exports(result.graph, out, root);
  const bool ok =
      std::filesystem::exists(out / "graph.json") &&
      std::filesystem::exists(out / "graph.html") &&
      std::filesystem::exists(out / "graph.svg") &&
      std::filesystem::exists(out / "obsidian.md") &&
      std::filesystem::exists(out / "cypher.txt") &&
      !std::filesystem::exists(out / "call-flow.html") &&  // replaced by the design report
      std::filesystem::exists(out / "modules.mmd") &&
      std::filesystem::exists(out / "modules.svg") &&
      std::filesystem::exists(out / "design.mmd") &&
      std::filesystem::exists(out / "design.md");

  std::filesystem::remove_all(root);
  std::filesystem::remove_all(out);
  if (!ok) {
    return 1;
  }

  // The whole pipeline, not just extraction, is location-independent: the same
  // tree built from a shallow and a deep root yields identical node ids and
  // identical edges, so two checkouts of one commit can be joined by id.
  const auto shallow = std::filesystem::temp_directory_path() / "cgraph_pipeline_shallow";
  const auto deep = std::filesystem::temp_directory_path() / "cgraph_pipeline_deep" / "a" / "much" / "deeper" / "checkout";
  const auto build_shape = [&](const std::filesystem::path& project_root) {
    std::filesystem::remove_all(project_root);
    write_file(project_root / "main.py", kPythonSource);
    write_file(project_root / "svc" / "service.go", kGoSource);
    write_file(project_root / "svc" / "index.ts", "import { helper } from \"./helper\";\nexport function run() { return helper(); }\n");
    write_file(project_root / "svc" / "helper.ts", "export function helper() { return 1; }\n");
    const auto built = cgraph::run_one_shot(project_root);
    std::vector<std::string> shape;
    for (const auto& node : built.graph.nodes) {
      shape.push_back("node " + node.id + " " + node.kind + " " + node.label);
    }
    for (const auto& edge : built.graph.edges) {
      shape.push_back("edge " + edge.source + " -" + edge.relation + "-> " + edge.target);
    }
    std::sort(shape.begin(), shape.end());
    return shape;
  };
  const auto shallow_shape = build_shape(shallow);
  const auto deep_shape = build_shape(deep);
  std::filesystem::remove_all(shallow);
  std::filesystem::remove_all(std::filesystem::temp_directory_path() / "cgraph_pipeline_deep");
  if (shallow_shape.size() < 6 || shallow_shape != deep_shape) {
    return 9;
  }
  for (const auto& line : shallow_shape) {
    if (line.find("cgraph_pipeline") != std::string::npos || line.find("much_deeper_checkout") != std::string::npos) {
      return 10;  // a segment above the project root leaked into an id or label
    }
  }
  return 0;
}
