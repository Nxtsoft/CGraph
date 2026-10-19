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

// Atomic; throws FileWriteError (atomic_write.hpp) if the file cannot be fully written.
//
// If `path` already holds a file read_semantic_cache cannot use (truncated, a
// malformed record, an unsupported version), that file is first moved to
// `<path>.invalid` instead of being overwritten, and the new location is
// returned so the caller can tell the user. If it cannot be moved the write
// throws and nothing is overwritten.
std::optional<std::filesystem::path> write_semantic_cache(
    const SemanticCache& cache,
    const std::filesystem::path& path);

// A missing file is an empty cache. A file that exists but cannot be used
// (not JSON, not schema v2, a malformed or duplicate record) also reads as an
// empty cache, never partially; when `problem` is given it receives the reason
// (and stays untouched for a missing or usable file).
[[nodiscard]] SemanticCache read_semantic_cache(
    const std::filesystem::path& path,
    std::string* problem = nullptr);

}  // namespace cgraph
