#pragma once

#include "cgraph/daemon_ops.hpp"
#include "cgraph/semantic_cache.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cgraph {

struct SemanticIngestResult {
  bool merged = false;
  bool cache_updated = false;
  std::vector<std::string> errors;
};

struct SemanticSourceInput {
  std::filesystem::path path;
  // Hash from the host-visible plan. Empty only for a legacy/unattributed drop
  // that is being validated from its current bytes for the first time.
  std::string content_sha256;
};

[[nodiscard]] SemanticIngestResult ingest_semantic_fragment(
    DaemonState& state,
    SemanticCache& cache,
    const std::vector<SemanticSourceInput>& sources,
    const std::filesystem::path& fragment_path);

void record_failed_ingest(
    SemanticCache& cache,
    const std::vector<SemanticSourceInput>& sources,
    const std::filesystem::path& fragment_path,
    const std::string& error,
    std::string_view fragment_sha256 = {});

}  // namespace cgraph
