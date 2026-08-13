#pragma once

#include "cgraph/file_cache.hpp"
#include "cgraph/semantic_cache.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace cgraph {

// Per-source stat cache for the enrichment plan: path (generic string) -> last
// seen {size, modified_at, sha256}. Lets planning reuse a stored hash when a
// file's size and mtime are unchanged, instead of re-reading and re-hashing it.
// Same primitive the deterministic code path uses (FileCacheEntry / StatHit).
using SemanticStatIndex = std::unordered_map<std::string, FileCacheEntry>;

enum class SemanticInputKind {
  Document,
  Media,
};

struct SemanticInput {
  std::filesystem::path path;
  SemanticInputKind kind = SemanticInputKind::Document;
  std::string content_hash;
  std::uintmax_t size = 0;
};

struct SemanticChunk {
  std::size_t index = 0;
  std::vector<SemanticInput> inputs;
};

struct SemanticChunkPlan {
  std::vector<SemanticChunk> chunks;
  std::size_t cache_hits = 0;
  std::size_t stale_inputs = 0;
  std::size_t failed_inputs = 0;
  std::size_t files_hashed = 0;
  std::size_t files_stat_reused = 0;
};

struct SemanticChunkPlanOptions {
  std::size_t max_files_per_chunk = 8;
  std::uintmax_t max_bytes_per_chunk = 1024 * 1024;
  // Directories never scanned for semantic inputs — the export output and the
  // semantic drop directory, so the tool never tries to enrich its own exports
  // or re-ingest already-dropped `chunk_NN.json` fragments as fresh documents.
  std::vector<std::filesystem::path> excluded_dirs;
};

// When `stat_index` is non-null, planning consults it to skip re-hashing
// unchanged files and updates it in place with the entries seen this pass. When
// null, every file is hashed (cold behavior). The produced plan is identical
// either way for a given tree + cache.
[[nodiscard]] SemanticChunkPlan plan_semantic_chunks(
    const std::filesystem::path& root,
    const SemanticCache& cache,
    SemanticChunkPlanOptions options = {},
    SemanticStatIndex* stat_index = nullptr);

void write_semantic_stat_index(const SemanticStatIndex& index, const std::filesystem::path& path);
[[nodiscard]] SemanticStatIndex read_semantic_stat_index(const std::filesystem::path& path);

}  // namespace cgraph
