#include "cgraph/incremental_update.hpp"

#include "cgraph/analysis.hpp"
#include "cgraph/configured_extractors.hpp"
#include "cgraph/content_root.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/dedup.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/file_extraction.hpp"
#include "cgraph/graph_builder.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace cgraph {

std::string incremental_file_key(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

namespace {

[[nodiscard]] GraphSnapshot rebuild_graph(const IncrementalGraphIndex& index) {
  std::vector<Fragment> fragments;
  std::vector<RawCall> raw_calls;
  std::vector<RawRelation> raw_relations;
  fragments.reserve(index.files.size());

  std::vector<std::string> keys;
  keys.reserve(index.files.size());
  for (const auto& [key, _] : index.files) {
    keys.push_back(key);
  }
  std::ranges::sort(keys);

  for (const auto& key : keys) {
    const auto& result = index.files.at(key);
    fragments.push_back(result.fragment);
    raw_calls.insert(raw_calls.end(), result.raw_calls.begin(), result.raw_calls.end());
    raw_relations.insert(raw_relations.end(), result.raw_relations.begin(), result.raw_relations.end());
  }

  auto graph = merge_fragments(fragments);
  graph.source_hashes.reserve(index.files.size());
  for (const auto& key : keys) {
    const auto& source_sha256 = index.files.at(key).source_sha256;
    if (!source_sha256.empty()) {
      graph.source_hashes.emplace(incremental_file_key(std::filesystem::path{key}), source_sha256);
    }
  }
  resolve_imports(graph, index.aliases);
  resolve_raw_calls(graph, raw_calls);
  resolve_raw_relations(graph, raw_relations);
  return graph;
}

[[nodiscard]] ContentRoot content_root_for(const IncrementalGraphIndex& index) {
  std::vector<FileCacheEntry> entries;
  entries.reserve(index.cache.size());
  for (const auto& [_, entry] : index.cache) {
    entries.push_back(entry);
  }
  return compute_content_root(index.project_root, entries);
}

// Community + centrality analysis, run AFTER dedup — exactly the order
// run_one_shot uses (merge -> resolve -> dedup -> communities -> analyze). Doing
// it here rather than inside rebuild_graph matters for two reasons:
//   * correctness/parity: dedup must see no community property, or it buckets
//     every node of a community together and runs O(k^2) fuzzy comparisons
//     within it (a ~60s cliff on a large repo); running before communities keeps
//     the daemon graph identical to the canonical one-shot graph.
//   * centrality is computed on the final, deduped node set, not a stale one.
void finalize_graph(GraphSnapshot& graph) {
  (void)detect_communities(graph);
  analyze_graph(graph);
}

// Keeps state.unextracted current across incremental adds/removes of files
// whose language has no registered extractor. Full rescans recompute the map
// wholesale, so any drift here self-heals on the next rescan.
void note_unextracted_change(DaemonState& state, const std::filesystem::path& path, bool added) {
  const auto language = detect_language(path);
  if (language == DetectedLanguage::Unknown || has_registered_extractor(language)) {
    return;
  }
  const auto name = std::string(language_name(language));
  // enrichment_mutex guards `unextracted` against a concurrent `status` read.
  const std::scoped_lock lock(state.enrichment_mutex);
  if (added) {
    ++state.unextracted[name];
    return;
  }
  if (const auto entry = state.unextracted.find(name); entry != state.unextracted.end()) {
    if (entry->second <= 1) {
      state.unextracted.erase(entry);
    } else {
      --entry->second;
    }
  }
}

}  // namespace

IncrementalUpdateResult full_stat_index_rescan(
    DaemonState& state,
    IncrementalGraphIndex& index,
    const std::filesystem::path& root,
    const IncrementalDedupPolicy& dedup_policy) {
  IncrementalUpdateResult result;
  result.full_rescan = true;

  auto detected_files = detect_project_files(root);

  // Phase 1 (read-only): content-verify every detected file against the
  // existing index. Nothing is mutated here, so discovering a previously
  // verified file that can no longer be read aborts the whole rescan
  // transactionally: the index, the coverage map, and the published graph all
  // keep their last verified state, and the caller learns why via warnings.
  struct Classified {
    DetectedFile file;
    std::string key;
    FileCacheClassification classification;
  };
  std::vector<Classified> classified;
  classified.reserve(detected_files.size());
  for (const auto& file : detected_files) {
    auto key = incremental_file_key(file.path);
    std::optional<FileCacheEntry> previous;
    if (const auto cached = index.cache.find(key); cached != index.cache.end()) {
      previous = cached->second;
    }
    auto classification =
        classify_cached_file(file.path, std::move(previous), CacheValidation::Content);
    if (classification.state == CacheState::Unreadable) {
      result.warnings.push_back("source unreadable: " + key);
      const auto had_verified_state = [&] {
        if (index.files.contains(key) || index.cache.contains(key)) {
          return true;
        }
        // The detection walk and the indexed events can disagree on key form
        // (e.g. a symlinked root), and an unreadable file defeats string-level
        // canonicalization -- fall back to filesystem identity.
        for (const auto& [_, entry] : index.cache) {
          std::error_code eq_error;
          if (std::filesystem::equivalent(entry.path, file.path, eq_error) && !eq_error) {
            return true;
          }
        }
        return false;
      }();
      if (had_verified_state) {
        // Verified state exists for these bytes; refusing the rescan is the
        // only way to keep it authoritative.
        result.warnings.push_back(
            "rescan refused, keeping last verified state: " + key);
        return result;
      }
      continue;  // a never-verified unreadable file is skipped, not fatal
    }
    classified.push_back(Classified{
        .file = file,  // detected_files stays intact for unextracted_counts below
        .key = std::move(key),
        .classification = std::move(classification),
    });
  }

  // Phase 2 (commit): from here on the index is rebuilt.
  index.project_root = std::filesystem::weakly_canonical(root);
  {
    // enrichment_mutex guards `unextracted` against a concurrent `status` read.
    const std::scoped_lock lock(state.enrichment_mutex);
    state.unextracted = unextracted_counts(detected_files);
  }
  std::unordered_map<std::string, ExtractionResult> rescanned;
  std::unordered_map<std::string, FileCacheEntry> rescanned_cache;
  rescanned.reserve(classified.size());
  rescanned_cache.reserve(classified.size());

  // A freshly computed hash hit whose extraction we already hold is reused
  // as-is; only changed and new files are re-extracted. With an empty index
  // (cold start) everything is "new" and we extract all; with a warm index we
  // re-extract only the delta. A reused fragment is byte-identical to
  // re-extracting it, so the merged graph is unchanged either way.
  std::vector<DetectedFile> to_extract;
  for (auto& [file, key, classification] : classified) {
    if (classification.state == CacheState::Deleted || !classification.current.has_value()) {
      continue;  // vanished between detect and classify; treat as removed
    }
    if (classification.hash_computed) {
      ++result.files_hashed;
      result.bytes_hashed += classification.current->size;
    }
    if (const auto existing = index.files.find(key);
        existing != index.files.end() && !existing->second.source_sha256.empty() &&
        existing->second.source_sha256 == classification.current->sha256) {
      rescanned.emplace(key, std::move(existing->second));
      rescanned_cache.emplace(key, *classification.current);
      ++result.files_cache_hit;
      continue;
    }
    rescanned_cache.emplace(key, *classification.current);
    to_extract.push_back(std::move(file));
  }

  // Re-extract only the changed/new files, concurrently. Time the extraction so
  // the modeled cache saving (files_reused x mean per-file extract time) can be
  // derived from real numbers in status.
  double extract_ms = 0.0;
  {
    ScopedTimer timer(&extract_ms);
    auto extractions = extract_files(to_extract);
    for (std::size_t i = 0; i < to_extract.size(); ++i) {
      auto& extraction = extractions[i];
      result.warnings.insert(result.warnings.end(), extraction.fragment.warnings.begin(), extraction.fragment.warnings.end());
      const auto key = incremental_file_key(to_extract[i].path);
      if (!extraction.source_sha256.empty()) {
        // The extractor opened the file independently of cache classification.
        // Publish the hash of the exact parsed source buffer so a concurrent
        // rewrite between those reads cannot separate the graph from its leaf.
        rescanned_cache.at(key).sha256 = extraction.source_sha256;
      }
      rescanned.emplace(key, std::move(extraction));
      ++result.files_reextracted;
    }
  }

  for (const auto& [key, _] : index.files) {
    if (!rescanned.contains(key)) {
      ++result.files_removed;
    }
  }

  index.files = std::move(rescanned);
  index.cache = std::move(rescanned_cache);
  index.updates_since_full_dedup = 0;
  index.aliases = load_path_aliases(root);

  const std::size_t files_total = result.files_reextracted + result.files_cache_hit;

  auto graph = rebuild_graph(index);
  semantic_dedup(graph, dedup_policy.options);
  finalize_graph(graph);  // communities + centrality on the deduped graph
  graph.cache_hit_rate = cache_hit_rate(result.files_cache_hit, files_total);
  graph.content_root = content_root_for(index);
  result.full_dedup_reconciled = true;

  // Record this build's Layer A inputs for the modeled cache-saving estimate.
  // Mean is left 0 (estimate suppressed) when nothing was extracted this build.
  state.last_files_cache_hit = result.files_cache_hit;
  state.last_extract_mean_ms =
      result.files_reextracted == 0 ? 0.0 : extract_ms / static_cast<double>(result.files_reextracted);

  publish_graph_snapshot(state, std::move(graph));
  return result;
}

IncrementalUpdateResult apply_incremental_code_updates(
    DaemonState& state,
    IncrementalGraphIndex& index,
    std::span<const FileWatchEvent> events,
    const IncrementalDedupPolicy& dedup_policy) {
  IncrementalUpdateResult result;
  std::vector<std::string> changed_sources;
  bool applied = false;  // any index/cache mutation; nothing applied -> no republish

  for (const auto& event : events) {
    if (event.change == FileWatchChange::Overflow) {
      return full_stat_index_rescan(state, index, event.path, dedup_policy);
    }
    if (event.kind != WatchedFileKind::Code) {
      continue;
    }

    const auto key = incremental_file_key(event.path);
    if (event.change == FileWatchChange::Deleted) {
      applied = index.cache.erase(key) > 0 || applied;
      if (index.files.erase(key) > 0) {
        applied = true;
        ++result.files_removed;
        changed_sources.push_back(key);
        note_unextracted_change(state, event.path, /*added=*/false);
      }
      continue;
    }

    const auto language = detect_language(event.path);
    if (language == DetectedLanguage::Unknown || !std::filesystem::exists(event.path)) {
      if (language == DetectedLanguage::Unknown && index.files.contains(key)) {
        applied = true;
        changed_sources.push_back(key);
        continue;
      }
      if (!std::filesystem::exists(event.path) && index.files.contains(key)) {
        applied = true;
        changed_sources.push_back(key);
        continue;
      }
      applied = index.cache.erase(key) > 0 || applied;
      if (index.files.erase(key) > 0) {
        applied = true;
        ++result.files_removed;
        changed_sources.push_back(key);
        note_unextracted_change(state, event.path, /*added=*/false);
      }
      continue;
    }

    std::optional<FileCacheEntry> previous_cache;
    if (const auto cached = index.cache.find(key); cached != index.cache.end()) {
      previous_cache = cached->second;
    }
    auto cache_classification =
        classify_cached_file(event.path, std::move(previous_cache), CacheValidation::Content);
    if (cache_classification.state == CacheState::Unreadable) {
      // Same contract as the full rescan: never replace the verified index,
      // cache, or graph state for a file whose current bytes cannot be read.
      result.warnings.push_back(
          "source unreadable, keeping last verified state: " + key);
      continue;
    }
    if (cache_classification.hash_computed && cache_classification.current.has_value()) {
      ++result.files_hashed;
      result.bytes_hashed += cache_classification.current->size;
    }
    if (cache_classification.current.has_value()) {
      index.cache[key] = *cache_classification.current;
      applied = true;
    }
    if (const auto existing = index.files.find(key);
        existing != index.files.end() && cache_classification.current.has_value() &&
        !existing->second.source_sha256.empty() &&
        existing->second.source_sha256 == cache_classification.current->sha256) {
      ++result.files_cache_hit;
      continue;
    }

    auto extraction = extract_detected_file(DetectedFile{.path = event.path, .language = language});
    result.warnings.insert(result.warnings.end(), extraction.fragment.warnings.begin(), extraction.fragment.warnings.end());
    if (!index.files.contains(key)) {
      note_unextracted_change(state, event.path, /*added=*/true);
    }
    if (!extraction.source_sha256.empty()) {
      if (const auto cached = index.cache.find(key); cached != index.cache.end()) {
        cached->second.sha256 = extraction.source_sha256;
      } else {
        // The file can vanish during classification and reappear before
        // extraction. Keep the exact parsed hash even when no classified entry
        // survived; metadata is advisory and the next content check refreshes it.
        index.cache.emplace(
            key,
            FileCacheEntry{.path = event.path, .sha256 = extraction.source_sha256});
      }
    }
    index.files[key] = std::move(extraction);
    applied = true;
    ++result.files_reextracted;
    changed_sources.push_back(key);
  }

  if (!applied) {
    // Every event was skipped (e.g. unreadable sources kept at their last
    // verified state): the published graph is still authoritative, so do not
    // rebuild or republish anything.
    return result;
  }

  auto graph = rebuild_graph(index);
  if (!changed_sources.empty()) {
    semantic_dedup_neighborhood(graph, changed_sources, dedup_policy.options);
    result.neighborhood_deduped = true;
    ++index.updates_since_full_dedup;
  }
  if (dedup_policy.full_reconcile_every > 0 && index.updates_since_full_dedup >= dedup_policy.full_reconcile_every) {
    semantic_dedup(graph, dedup_policy.options);
    result.full_dedup_reconciled = true;
    index.updates_since_full_dedup = 0;
  }
  finalize_graph(graph);  // communities + centrality after dedup (matches run_one_shot order)
  graph.content_root = content_root_for(index);
  publish_graph_snapshot(state, std::move(graph));
  return result;
}

}  // namespace cgraph
