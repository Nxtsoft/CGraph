#include "cgraph/pipeline.hpp"

#include "cgraph/analysis.hpp"
#include "cgraph/configured_extractors.hpp"
#include "cgraph/dedup.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/export_json.hpp"
#include "cgraph/file_extraction.hpp"
#include "cgraph/graph_builder.hpp"

#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cgraph {
namespace {

void write_text(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

}  // namespace

PipelineResult run_one_shot(const std::filesystem::path& root) {
  PipelineResult result;
  const auto files = detect_project_files(root);
  result.file_count = files.size();
  // A one-shot build is a cold build: every file is extracted, none reused.
  result.stats.files_total = files.size();
  result.stats.files_extracted = files.size();
  result.stats.files_cache_hit = 0;
  result.stats.unextracted = unextracted_counts(files);

  std::vector<Fragment> fragments;
  std::vector<RawCall> raw_calls;
  std::vector<RawRelation> raw_relations;
  std::unordered_map<std::string, std::string> source_hashes;
  fragments.reserve(files.size());
  source_hashes.reserve(files.size());

  // Time each phase at the orchestration seam (scoped timers), so instrumentation
  // stays here rather than threaded into every extractor/builder/dedup function.
  {
    ScopedTimer timer(&result.stats.extract_ms);
    // Extract concurrently; the results come back in detection order, so merging
    // them below is identical to the serial path (parity preserved).
    auto extractions = extract_files(files);
    for (std::size_t i = 0; i < extractions.size(); ++i) {
      auto& extraction = extractions[i];
      result.warnings.insert(result.warnings.end(), extraction.fragment.warnings.begin(), extraction.fragment.warnings.end());
      raw_calls.insert(raw_calls.end(), extraction.raw_calls.begin(), extraction.raw_calls.end());
      raw_relations.insert(raw_relations.end(), extraction.raw_relations.begin(), extraction.raw_relations.end());
      if (!extraction.source_sha256.empty()) {
        source_hashes.emplace(
            files[i].path.lexically_normal().generic_string(), extraction.source_sha256);
      }
      fragments.push_back(std::move(extraction.fragment));
    }
  }

  {
    ScopedTimer timer(&result.stats.merge_ms);
    result.graph = merge_fragments(fragments);
    result.graph.source_hashes = std::move(source_hashes);
  }
  {
    ScopedTimer timer(&result.stats.resolve_ms);
    const auto aliases = load_path_aliases(root);
    resolve_imports(result.graph, aliases);
    resolve_raw_calls(result.graph, raw_calls, &result.stats.calls);
    resolve_raw_relations(result.graph, raw_relations);
    resolve_interface_dispatch(result.graph, raw_calls);
  }
  {
    ScopedTimer timer(&result.stats.dedup_ms);
    semantic_dedup(result.graph);
  }
  {
    ScopedTimer timer(&result.stats.communities_ms);
    const auto community_result = detect_communities(result.graph);
    if (community_result.cluster_count == 0 && !result.graph.nodes.empty()) {
      result.warnings.push_back("community detection produced no clusters");
    }
  }
  {
    ScopedTimer timer(&result.stats.analyze_ms);
    analyze_graph(result.graph);
  }

  result.stats.nodes = result.graph.nodes.size();
  result.stats.edges = result.graph.edges.size();
  result.graph.cache_hit_rate = cache_hit_rate(result.stats.files_cache_hit, result.stats.files_total);
  return result;
}

void write_exports(const GraphSnapshot& graph, const std::filesystem::path& output_dir) {
  std::filesystem::create_directories(output_dir);
  write_text(output_dir / "graph.json", to_node_link_json(graph).dump(2));
  write_text(output_dir / "graph.html", export_graph_html(graph));
  write_text(output_dir / "graph.svg", export_graph_svg(graph));
  write_text(output_dir / "obsidian.md", export_obsidian_markdown(graph));
  write_text(output_dir / "cypher.txt", export_neo4j_cypher(graph));
  write_text(output_dir / "call-flow.html", export_call_flow_html(graph));
}

}  // namespace cgraph
