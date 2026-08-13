#pragma once

#include "cgraph/semantic_chunk_plan.hpp"
#include "cgraph/semantic_ingest.hpp"
#include "cgraph/types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace cgraph {

// The default directory hosts drop computed `chunk_NN.json` fragments into,
// derived from the export output directory.
[[nodiscard]] std::filesystem::path default_semantic_drop_dir(const std::filesystem::path& output_dir);

struct EnrichmentPlanResult {
  SemanticChunkPlan plan;
  std::filesystem::path drop_dir;
  std::filesystem::path manifest_path;  // <drop>/plan.json
  std::size_t inputs_to_enrich = 0;     // total inputs across all chunks
};

// Emits a semantic chunk plan for `root`, skipping content already covered by a
// valid cache record in `drop_dir`. Writes a host-readable manifest
// (`plan.json`: each chunk's index, the fragment filename to drop, and its
// source inputs with content hashes) so a host can dispatch each chunk and drop
// back exactly one `chunk_NN.json`. Creates `drop_dir` if absent.
[[nodiscard]] EnrichmentPlanResult plan_enrichment(
    const std::filesystem::path& root,
    const std::filesystem::path& drop_dir);

struct EnrichmentIngestResult {
  GraphSnapshot graph;                  // deterministic graph with merged fragments
  std::size_t fragments_ingested = 0;
  std::size_t fragments_rejected = 0;
  std::size_t deterministic_nodes = 0;  // node count before enrichment
  std::vector<std::string> errors;
};

// Builds the deterministic graph for `root`, then validates and merges every
// `chunk_NN.json` present in `drop_dir` through the daemon single-writer path,
// updating (and persisting) the semantic cache keyed by content hash. Source
// attribution for each fragment comes from the `plan.json` manifest. Malformed
// fragments are rejected and leave the graph unchanged.
[[nodiscard]] EnrichmentIngestResult ingest_enrichment(
    const std::filesystem::path& root,
    const std::filesystem::path& drop_dir);

// The fragment filename a host must drop for a given chunk index ("chunk_03.json").
[[nodiscard]] std::string fragment_filename_for_chunk(std::size_t chunk_index);

// Maps each chunk index to its source input paths, read from the drop dir's
// `plan.json` manifest (empty when absent). Used to attribute an ingested
// fragment to the document(s) it enriches when updating the content-hash cache.
[[nodiscard]] std::unordered_map<std::size_t, std::vector<SemanticSourceInput>> load_chunk_sources(
    const std::filesystem::path& drop_dir);

}  // namespace cgraph
