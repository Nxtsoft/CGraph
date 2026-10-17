#include "cgraph/semantic_orchestration.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

[[nodiscard]] bool has_label(const cgraph::GraphSnapshot& graph, const std::string& label) {
  for (const auto& node : graph.nodes) {
    if (node.label == label) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "cgraph_semantic_orchestration_test";
  fs::remove_all(root);
  fs::create_directories(root);

  // A real little project: one code file (deterministic graph) and one doc (a
  // semantic input a host would enrich).
  write_file(root / "src" / "service.ts", "export function runService() { return 1; }\n");
  write_file(root / "docs" / "overview.md", "# Overview\nThe service runs the pipeline.\n");

  const auto drop = cgraph::default_semantic_drop_dir(root / "out");

  // Plan: the doc is uncached, so it must appear as a chunk; the code file must
  // not (code extraction stays deterministic).
  const auto plan = cgraph::plan_enrichment(root, drop);
  if (plan.plan.chunks.size() != 1 || plan.inputs_to_enrich != 1) {
    return 1;
  }
  bool doc_planned = false;
  for (const auto& input : plan.plan.chunks.front().inputs) {
    doc_planned = doc_planned || input.path.filename() == "overview.md";
  }
  if (!doc_planned || !fs::exists(plan.manifest_path)) {
    return 1;
  }

  // No <out>/graph.json exists here, so candidate_links degrades to empty and the
  // plan is otherwise unchanged (additive feature).
  {
    std::ifstream manifest_file(plan.manifest_path, std::ios::binary);
    const auto manifest = nlohmann::json::parse(manifest_file, nullptr, false);
    const auto& input0 = manifest["chunks"][0]["inputs"][0];
    if (!input0.contains("candidate_links") || !input0["candidate_links"].empty()) {
      return 1;  // present but empty when no code graph is available
    }
  }

  // Host authors the fragment for chunk 0 and drops it.
  const auto fragment = drop / cgraph::fragment_filename_for_chunk(plan.plan.chunks.front().index);
  write_file(fragment, R"({
    "nodes": [
      {"id": "doc:overview", "label": "Overview", "type": "document", "source_file": "docs/overview.md"},
      {"id": "concept:pipeline", "label": "Pipeline", "type": "concept", "source_file": "docs/overview.md"}
    ],
    "edges": [
      {"source": "doc:overview", "target": "concept:pipeline", "relation": "DESCRIBES"}
    ]
  })");

  // Ingest: deterministic graph gains the two semantic nodes + edge.
  const auto ingest = cgraph::ingest_enrichment(root, drop);
  if (ingest.fragments_ingested != 1 || ingest.fragments_rejected != 0) {
    return 1;
  }
  if (ingest.graph.nodes.size() != ingest.deterministic_nodes + 2) {
    return 1;
  }
  if (!has_label(ingest.graph, "Overview") || !has_label(ingest.graph, "Pipeline") ||
      !has_label(ingest.graph, "runService")) {
    return 1;
  }

  // Second pass: add another doc. The cached doc is skipped; the new doc is a
  // fresh chunk. Crucially, its fragment name must be OFFSET past the already
  // dropped chunk_00.json, so a second pass cannot overwrite the first pass's
  // fragment — the drop directory accumulates and ingest stays additive.
  write_file(root / "docs" / "architecture.md", "# Architecture\nThe daemon serves queries.\n");
  const auto replan = cgraph::plan_enrichment(root, drop);
  if (replan.plan.chunks.size() != 1 || replan.plan.cache_hits != 1) {
    return 1;  // overview cached, architecture pending
  }
  {
    std::ifstream manifest_file(replan.manifest_path, std::ios::binary);
    const auto manifest = nlohmann::json::parse(manifest_file, nullptr, false);
    const auto fragment_name = manifest["chunks"][0].value("fragment", std::string{});
    if (fragment_name != cgraph::fragment_filename_for_chunk(1)) {
      return 1;  // offset applied: chunk_01.json, not a colliding chunk_00.json
    }
  }
  write_file(drop / cgraph::fragment_filename_for_chunk(1), R"({
    "nodes": [
      {"id": "doc:arch", "label": "ArchDoc", "type": "document"},
      {"id": "concept:daemon", "label": "DaemonConcept", "type": "concept"}
    ],
    "edges": [{"source": "doc:arch", "target": "concept:daemon", "relation": "DESCRIBES"}]
  })");

  // Ingest re-applies BOTH fragments: the first pass's nodes are NOT lost.
  const auto ingest2 = cgraph::ingest_enrichment(root, drop);
  if (ingest2.fragments_ingested != 2 || ingest2.fragments_rejected != 0) {
    return 1;
  }
  if (!has_label(ingest2.graph, "Overview") || !has_label(ingest2.graph, "Pipeline") ||
      !has_label(ingest2.graph, "ArchDoc") || !has_label(ingest2.graph, "DaemonConcept")) {
    return 1;  // both passes coexist
  }
  if (ingest2.graph.nodes.size() != ingest2.deterministic_nodes + 4) {
    return 1;
  }

  // A malformed fragment dropped as a new file is rejected and leaves the graph
  // unchanged; the two valid fragments still merge.
  write_file(drop / cgraph::fragment_filename_for_chunk(2), R"({"nodes":[{"id":"no-label"}],"edges":[]})");
  const auto bad = cgraph::ingest_enrichment(root, drop);
  if (bad.fragments_rejected != 1 || bad.fragments_ingested != 2 || bad.errors.empty()) {
    return 1;
  }
  if (bad.graph.nodes.size() != bad.deterministic_nodes + 4) {
    return 1;  // rejected fragment adds nothing; the valid ones persist
  }

  // --- candidate doc->code links: a persisted code graph lets the plan hand the
  // host real code-node ids for symbols the doc mentions. ---
  {
    const auto croot = fs::temp_directory_path() / "cgraph_candidate_links_test";
    fs::remove_all(croot);
    const auto cdrop = cgraph::default_semantic_drop_dir(croot / "out");
    // Persisted code graph with a compound-named symbol node (the <out> dir is
    // excluded from the doc walk, so graph.json is never planned as a document).
    write_file(croot / "out" / "graph.json", R"JSON({
      "directed": true, "multigraph": false, "graph": {},
      "nodes": [{"id": "code:ccf", "label": "classify_cached_file(path)", "type": "function"}],
      "links": []
    })JSON");
    // A doc that names that symbol.
    write_file(croot / "docs" / "cache.md", "# Cache\nThe walk calls classify_cached_file before hashing.\n");

    const auto cplan = cgraph::plan_enrichment(croot, cdrop);
    std::ifstream manifest_file(cplan.manifest_path, std::ios::binary);
    const auto manifest = nlohmann::json::parse(manifest_file, nullptr, false);
    bool linked = false;
    for (const auto& chunk : manifest["chunks"]) {
      for (const auto& input : chunk["inputs"]) {
        if (input.value("path", std::string{}).find("cache.md") == std::string::npos) {
          continue;
        }
        for (const auto& link : input.value("candidate_links", nlohmann::json::array())) {
          if (link.value("id", std::string{}) == "code:ccf") {
            linked = true;
          }
        }
      }
    }
    if (!linked) {
      return 2;  // the doc's input must carry the real code-node id it mentions
    }
    manifest_file.close();  // Windows cannot delete a file that is still open
    fs::remove_all(croot);
  }

  // --- non-contiguous existing fragments must not collide on a new pass ---
  // With chunk_00 + chunk_05 on disk (a gap), a count-based offset would yield
  // chunk_02 and could overwrite a later fragment; max+1 must yield chunk_06+.
  {
    const auto groot = fs::temp_directory_path() / "cgraph_fragment_gap_test";
    fs::remove_all(groot);
    const auto gdrop = cgraph::default_semantic_drop_dir(groot / "out");
    write_file(gdrop / "chunk_00.json", R"({"nodes":[],"edges":[]})");
    write_file(gdrop / "chunk_05.json", R"({"nodes":[],"edges":[]})");
    write_file(groot / "docs" / "fresh.md", "# Fresh\nA brand new uncached document.\n");

    const auto gap_plan = cgraph::plan_enrichment(groot, gdrop);
    std::ifstream manifest_file(gap_plan.manifest_path, std::ios::binary);
    const auto manifest = nlohmann::json::parse(manifest_file, nullptr, false);
    const auto fragment_name = manifest["chunks"][0].value("fragment", std::string{});
    // Strictly past the highest existing index (5): chunk_06.json or higher,
    // and never an existing filename.
    if (fragment_name != cgraph::fragment_filename_for_chunk(6)) {
      return 3;
    }
    if (fs::exists(gdrop / fragment_name)) {
      return 4;  // must not collide with anything already on disk
    }
    manifest_file.close();  // Windows cannot delete a file that is still open
    fs::remove_all(groot);
  }

  fs::remove_all(root);
  return 0;
}
