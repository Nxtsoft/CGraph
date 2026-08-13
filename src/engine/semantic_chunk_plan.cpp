#include "cgraph/semantic_chunk_plan.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/file_watcher.hpp"
#include "cgraph/path_ignore.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>

namespace cgraph {
namespace {

// file_time_type <-> integer ticks, matching the code-side index manifest
// serialization so the same FileCacheEntry round-trips identically.
[[nodiscard]] std::int64_t mtime_count(const std::filesystem::file_time_type& time) {
  return static_cast<std::int64_t>(time.time_since_epoch().count());
}

[[nodiscard]] std::filesystem::file_time_type mtime_from_count(std::int64_t count) {
  return std::filesystem::file_time_type(std::filesystem::file_time_type::duration(count));
}

[[nodiscard]] bool is_excluded_dir(
    const std::filesystem::path& dir,
    const std::vector<std::filesystem::path>& excluded) {
  std::error_code ec;
  for (const auto& candidate : excluded) {
    if (std::filesystem::equivalent(dir, candidate, ec)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::vector<std::filesystem::path> collect_semantic_paths(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& excluded_dirs) {
  std::vector<std::filesystem::path> paths;
  const auto canonical_root = std::filesystem::weakly_canonical(root);
  const auto gitignore_patterns = read_root_gitignore(canonical_root);
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      canonical_root,
      std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::recursive_directory_iterator end;

  for (; !error && iterator != end; iterator.increment(error)) {
    const auto& entry = *iterator;
    if (entry.is_directory(error)) {
      // is_dependency_directory subsumes the name skip-list and adds the
      // structural markers (pyvenv.cfg, linked git-worktree `.git` file), so the
      // enrichment planner skips the same trees as detection and the watcher.
      if (is_dependency_directory(entry.path()) || is_excluded_dir(entry.path(), excluded_dirs) ||
          matches_simple_gitignore(canonical_root, entry.path(), gitignore_patterns)) {
        iterator.disable_recursion_pending();
      }
      continue;
    }
    if (!entry.is_regular_file(error) || !is_watchable_file(entry.path())) {
      continue;
    }
    if (matches_simple_gitignore(canonical_root, entry.path(), gitignore_patterns)) {
      continue;
    }
    if (classify_watched_file(entry.path()) == WatchedFileKind::Code) {
      continue;
    }
    paths.push_back(entry.path());
  }

  std::ranges::sort(paths, [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  return paths;
}

[[nodiscard]] SemanticInputKind semantic_kind_for(const std::filesystem::path& path) {
  return classify_watched_file(path) == WatchedFileKind::Media ? SemanticInputKind::Media : SemanticInputKind::Document;
}

[[nodiscard]] bool is_valid_cached_record(const SemanticCacheRecord& record, std::string_view current_hash) {
  return record.state == SemanticCacheState::Valid &&
         record.content_hash == current_hash &&
         std::filesystem::exists(record.fragment_path) &&
         !record.fragment_hash.empty() &&
         sha256_file_hex(record.fragment_path) == record.fragment_hash;
}

void flush_chunk(SemanticChunkPlan& plan, SemanticChunk& chunk, std::uintmax_t& chunk_bytes) {
  if (chunk.inputs.empty()) {
    return;
  }
  chunk.index = plan.chunks.size();
  plan.chunks.push_back(std::move(chunk));
  chunk = SemanticChunk{};
  chunk_bytes = 0;
}

}  // namespace

SemanticChunkPlan plan_semantic_chunks(
    const std::filesystem::path& root,
    const SemanticCache& cache,
    SemanticChunkPlanOptions options,
    SemanticStatIndex* stat_index) {
  SemanticChunkPlan plan;
  SemanticChunk current_chunk;
  std::uintmax_t current_bytes = 0;

  // When no caller-owned index is supplied (cold one-shot), use a throwaway so
  // the loop stays a single path: a missing entry classifies as New and hashes,
  // exactly the prior behavior.
  SemanticStatIndex local_index;
  SemanticStatIndex& index = stat_index != nullptr ? *stat_index : local_index;

  for (const auto& path : collect_semantic_paths(root, options.excluded_dirs)) {
    const auto key = path.generic_string();
    std::optional<FileCacheEntry> previous;
    if (const auto it = index.find(key); it != index.end()) {
      previous = it->second;
    }
    // Stat first: a StatHit reuses the stored hash with no read; only new or
    // changed files are read and SHA-256'd.
    const auto classification = classify_cached_file(path, previous);
    if (classification.state == CacheState::Deleted || !classification.current.has_value()) {
      continue;  // vanished between walk and stat
    }
    if (classification.hash_computed) {
      ++plan.files_hashed;
    } else {
      ++plan.files_stat_reused;
    }
    index[key] = *classification.current;

    const auto& hash = classification.current->sha256;
    const auto cached = cache.find_for_source(path);
    const auto current = cache.find_for_source(path, hash);
    if (current.has_value() && is_valid_cached_record(*current, hash)) {
      ++plan.cache_hits;
      continue;
    }
    if (cached.has_value()) {
      if (current.has_value() && current->state == SemanticCacheState::Failed) {
        ++plan.failed_inputs;
      } else {
        ++plan.stale_inputs;
      }
    }

    const auto input = SemanticInput{
        .path = path,
        .kind = semantic_kind_for(path),
        .content_hash = hash,
        .size = classification.current->size,
    };

    const auto would_exceed_file_count =
        options.max_files_per_chunk > 0 && current_chunk.inputs.size() >= options.max_files_per_chunk;
    const auto would_exceed_bytes =
        options.max_bytes_per_chunk > 0 && !current_chunk.inputs.empty() &&
        current_bytes + input.size > options.max_bytes_per_chunk;
    if (would_exceed_file_count || would_exceed_bytes) {
      flush_chunk(plan, current_chunk, current_bytes);
    }

    current_bytes += input.size;
    current_chunk.inputs.push_back(input);
  }

  flush_chunk(plan, current_chunk, current_bytes);
  return plan;
}

void write_semantic_stat_index(const SemanticStatIndex& index, const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  // Emit in sorted key order so the file is deterministic across writes.
  std::vector<std::string> keys;
  keys.reserve(index.size());
  for (const auto& [key, _] : index) {
    keys.push_back(key);
  }
  std::ranges::sort(keys);

  auto entries = nlohmann::json::array();
  for (const auto& key : keys) {
    const auto& entry = index.at(key);
    entries.push_back({
        {"path", entry.path.generic_string()},
        {"size", entry.size},
        {"modified_at", mtime_count(entry.modified_at)},
        {"sha256", entry.sha256},
    });
  }
  std::ofstream output(path, std::ios::binary);
  output << nlohmann::json{{"version", 1}, {"entries", std::move(entries)}}.dump(2);
}

SemanticStatIndex read_semantic_stat_index(const std::filesystem::path& path) {
  SemanticStatIndex index;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return index;  // absent -> cold, no error
  }
  const auto root = nlohmann::json::parse(input, nullptr, false);
  if (!root.is_object() || !root.contains("entries") || !root["entries"].is_array()) {
    return index;  // malformed -> cold, no error
  }
  for (const auto& item : root["entries"]) {
    FileCacheEntry entry;
    entry.path = std::filesystem::path(item.value("path", std::string{}));
    entry.size = item.value("size", std::uintmax_t{0});
    entry.modified_at = mtime_from_count(item.value("modified_at", std::int64_t{0}));
    entry.sha256 = item.value("sha256", std::string{});
    if (!entry.path.empty() && !entry.sha256.empty()) {
      index.emplace(entry.path.generic_string(), std::move(entry));
    }
  }
  return index;
}

}  // namespace cgraph
