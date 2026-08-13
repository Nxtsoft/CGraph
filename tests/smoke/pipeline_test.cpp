#include "cgraph/operation_stats.hpp"
#include "cgraph/pipeline.hpp"
#include "cgraph/file_cache.hpp"

#include <filesystem>
#include <fstream>

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
  write_file(root / "legacy.cs", "class Legacy { void Run() {} }\n");

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

  if (result.stats.unextracted.size() != 1 || result.stats.unextracted.at("csharp") != 1) {
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
      stats_json["unextracted"]["csharp"] != 1) {
    std::filesystem::remove_all(root);
    return 5;
  }

  cgraph::write_exports(result.graph, out);
  const bool ok =
      std::filesystem::exists(out / "graph.json") &&
      std::filesystem::exists(out / "graph.html") &&
      std::filesystem::exists(out / "graph.svg") &&
      std::filesystem::exists(out / "obsidian.md") &&
      std::filesystem::exists(out / "cypher.txt") &&
      std::filesystem::exists(out / "call-flow.html");

  std::filesystem::remove_all(root);
  std::filesystem::remove_all(out);
  return ok ? 0 : 1;
}
