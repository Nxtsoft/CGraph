#pragma once

#include "cgraph/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cgraph {

enum class SemanticCacheState {
  Valid,
  Stale,
  Failed,
};

struct SemanticDependency {
  std::string node_id;
  std::string source_path;
  std::string source_sha256;
};

struct SemanticCacheRecord {
  std::filesystem::path source_path;
  std::string content_hash;
  std::filesystem::path fragment_path;
  std::string fragment_hash;
  SemanticCacheState state = SemanticCacheState::Valid;
  std::string last_error;
  std::vector<SemanticDependency> dependencies;
};

[[nodiscard]] std::string normalize_semantic_source_path(
    const std::filesystem::path& path);

class SemanticCache {
 public:
  void upsert(SemanticCacheRecord record);
  void remove(const std::filesystem::path& source_path);
  [[nodiscard]] std::optional<SemanticCacheRecord> find_for_source(const std::filesystem::path& path) const;
  [[nodiscard]] std::optional<SemanticCacheRecord> find_for_source(
      const std::filesystem::path& path,
      std::string_view content_hash) const;
  [[nodiscard]] std::vector<SemanticCacheRecord> find_for_fragment(const std::filesystem::path& fragment_path) const;
  [[nodiscard]] std::vector<SemanticCacheRecord> records() const;
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] std::size_t count_valid() const;
  [[nodiscard]] std::size_t count_stale() const;
  [[nodiscard]] std::size_t count_failed() const;

 private:
  std::unordered_map<std::string, SemanticCacheRecord> records_by_source_;
};

struct ReconciliationResult {
  std::size_t records_checked = 0;
  std::size_t records_invalidated = 0;
  std::size_t records_preserved = 0;
};

[[nodiscard]] ReconciliationResult reconcile_semantic_cache(
    SemanticCache& cache,
    const GraphSnapshot& graph);

void write_semantic_cache(const SemanticCache& cache, const std::filesystem::path& path);
[[nodiscard]] SemanticCache read_semantic_cache(const std::filesystem::path& path);

}  // namespace cgraph
