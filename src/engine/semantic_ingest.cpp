#include "cgraph/semantic_ingest.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/file_watcher.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/semantic_fragment_validation.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cgraph {
namespace {

struct DependencyResult {
  std::vector<SemanticDependency> dependencies;
  std::string error;
};

struct SourceFingerprint {
  std::filesystem::path path;
  std::string sha256;
};

[[nodiscard]] std::string normalized_path(const std::filesystem::path& path) {
  return normalize_semantic_source_path(path);
}

[[nodiscard]] std::string ascii_casefold(std::string_view value) {
  std::string folded;
  folded.reserve(value.size());
  for (const char ch : value) {
    folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return folded;
}

// Stamp the planned kind onto every node whose source_file is one of this
// fragment's planned enrichment sources, so a node's cross-file dedup identity
// does not depend on the host-optional `type`/`kind` field. The kind is
// re-derived from the source PATH via the same classifier the plan uses
// (classify_watched_file), so it is correct on every ingest path -- live drop,
// manifest replay, and restart replay from cache records that carry no kind.
//
// The host-written kind is overridden EXCEPT when the host explicitly marked the
// node a concept: a concept may legitimately carry a source_file (provenance of
// where the idea was discussed) and is a distinct semantic entity, not the
// document itself, so its identity must be preserved. A document a host wrongly
// labels `concept` is thereby left unprotected -- indistinguishable from a real
// concept, it is an accepted residual, not something to guess at.
void stamp_planned_kinds(
    std::vector<Node>& nodes,
    const std::unordered_set<std::string>& planned_sources) {
  for (auto& node : nodes) {
    if (node.source_file.empty() || !planned_sources.contains(node.source_file)) {
      continue;
    }
    if (ascii_casefold(node.kind) == "concept") {
      continue;
    }
    node.kind = classify_watched_file(node.source_file) == WatchedFileKind::Media
                    ? "media"
                    : "document";
  }
}

[[nodiscard]] DependencyResult compute_external_dependencies(
    const Fragment& fragment,
    const GraphSnapshot& graph) {
  std::unordered_set<std::string> fragment_node_ids;
  fragment_node_ids.reserve(fragment.nodes.size());
  for (const auto& node : fragment.nodes) {
    fragment_node_ids.insert(node_key(node));
  }

  std::unordered_map<std::string, const Node*> graph_nodes;
  graph_nodes.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    graph_nodes.emplace(node.id, &node);
  }

  std::unordered_set<std::string> seen;
  DependencyResult result;
  const auto record_endpoint = [&](const std::string& endpoint,
                                   std::string_view endpoint_kind) {
    if (fragment_node_ids.contains(endpoint) || !seen.insert(endpoint).second) {
      return true;
    }
    const auto node = graph_nodes.find(endpoint);
    if (node == graph_nodes.end()) {
      result.error = "semantic fragment " + std::string{endpoint_kind} +
                     " references unknown node: " + endpoint;
      return false;
    }
    const auto source_path = normalized_path(node->second->source_file);
    if (source_path.empty()) {
      result.error = "semantic fragment dependency has no source path: " + endpoint;
      return false;
    }
    const auto source_hash = graph.source_hashes.find(source_path);
    if (source_hash == graph.source_hashes.end() || source_hash->second.empty()) {
      result.error =
          "semantic fragment dependency has no snapshot source hash: " + endpoint;
      return false;
    }
    result.dependencies.push_back(SemanticDependency{
        .node_id = endpoint,
        .source_path = source_path,
        .source_sha256 = source_hash->second,
    });
    return true;
  };

  for (const auto& edge : fragment.edges) {
    for (const auto& endpoint : {edge.source, edge.target}) {
      if (!record_endpoint(endpoint, "edge")) {
        return result;
      }
    }
  }
  for (const auto& hyperedge : fragment.hyperedges) {
    for (const auto& endpoint : hyperedge.nodes) {
      if (!record_endpoint(endpoint, "hyperedge")) {
        return result;
      }
    }
  }
  std::ranges::sort(
      result.dependencies,
      {},
      &SemanticDependency::node_id);
  return result;
}

[[nodiscard]] std::vector<SourceFingerprint> fingerprint_sources(
    const std::vector<SemanticSourceInput>& sources,
    std::string& error) {
  std::vector<SourceFingerprint> fingerprints;
  fingerprints.reserve(sources.size());
  std::unordered_set<std::string> seen;
  for (const auto& source : sources) {
    const auto normalized = normalized_path(source.path);
    if (normalized.empty() || !seen.insert(normalized).second) {
      continue;
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(source.path, exists_error) || exists_error) {
      error = "semantic source file missing: " + normalized;
      return {};
    }
    const auto current_sha256 = sha256_file_hex(source.path);
    if (current_sha256.empty()) {
      error = "semantic source file could not be hashed: " + normalized;
      return {};
    }
    if (!source.content_sha256.empty() && source.content_sha256 != current_sha256) {
      error = "semantic source hash changed since planning: " + normalized;
      return {};
    }
    fingerprints.push_back(SourceFingerprint{
        .path = std::filesystem::path{normalized},
        .sha256 = current_sha256,
    });
  }
  if (fingerprints.empty()) {
    error = "semantic fragment has no source inputs";
  }
  return fingerprints;
}

void remove_obsolete_source_mappings(
    SemanticCache& cache,
    const std::filesystem::path& fragment_path,
    const std::unordered_set<std::string>& current_sources) {
  for (const auto& prior : cache.find_for_fragment(fragment_path)) {
    if (!current_sources.contains(normalized_path(prior.source_path))) {
      cache.remove(prior.source_path);
    }
  }
}

}  // namespace

SemanticIngestResult ingest_semantic_fragment(
    DaemonState& state,
    SemanticCache& cache,
    const std::vector<SemanticSourceInput>& sources,
    const std::filesystem::path& fragment_path) {
  SemanticIngestResult result;
  auto validation = validate_semantic_fragment_file(fragment_path);
  if (!validation.valid) {
    result.errors = std::move(validation.errors);
    record_failed_ingest(
        cache,
        sources,
        fragment_path,
        result.errors.empty() ? "validation failed" : result.errors.front(),
        validation.source_sha256);
    result.cache_updated = !sources.empty();
    return result;
  }
  for (auto& node : validation.fragment.nodes) {
    if (!node.source_file.empty()) {
      node.source_file = normalized_path(node.source_file);
    }
  }

  std::string source_error;
  auto source_fingerprints = fingerprint_sources(sources, source_error);
  if (!source_error.empty()) {
    result.errors.push_back(source_error);
    record_failed_ingest(
        cache, sources, fragment_path, source_error, validation.source_sha256);
    result.cache_updated = !sources.empty();
    return result;
  }

  std::unordered_set<std::string> current_sources;
  current_sources.reserve(source_fingerprints.size());
  for (const auto& source : source_fingerprints) {
    current_sources.insert(normalized_path(source.path));
  }

  // node.source_file was normalized in the loop above, so it matches the
  // normalized planned-source keys directly.
  stamp_planned_kinds(validation.fragment.nodes, current_sources);

  DependencyResult dependency_result;
  mutate_graph_snapshot(state, [&](GraphSnapshot& current) {
    dependency_result = compute_external_dependencies(validation.fragment, current);
    if (!dependency_result.error.empty()) {
      return;
    }
    merge_fragment(current, validation.fragment);
    for (const auto& source : source_fingerprints) {
      current.source_hashes[normalized_path(source.path)] = source.sha256;
    }
    current.source_hashes[normalized_path(fragment_path)] = validation.source_sha256;
  });
  if (!dependency_result.error.empty()) {
    result.errors.push_back(dependency_result.error);
    record_failed_ingest(
        cache,
        sources,
        fragment_path,
        dependency_result.error,
        validation.source_sha256);
    result.cache_updated = !sources.empty();
    return result;
  }
  result.merged = true;

  remove_obsolete_source_mappings(cache, fragment_path, current_sources);
  for (const auto& source : source_fingerprints) {
    SemanticCacheRecord record;
    record.source_path = source.path;
    record.content_hash = source.sha256;
    record.fragment_path = fragment_path;
    record.fragment_hash = validation.source_sha256;
    record.state = SemanticCacheState::Valid;
    record.dependencies = dependency_result.dependencies;
    cache.upsert(std::move(record));
  }
  result.cache_updated = true;
  return result;
}

void record_failed_ingest(
    SemanticCache& cache,
    const std::vector<SemanticSourceInput>& sources,
    const std::filesystem::path& fragment_path,
    const std::string& error,
    std::string_view fragment_sha256) {
  std::string current_fragment_hash{fragment_sha256};
  if (current_fragment_hash.empty()) {
    std::error_code exists_error;
    if (std::filesystem::exists(fragment_path, exists_error) && !exists_error) {
      current_fragment_hash = sha256_file_hex(fragment_path);
    }
  }
  std::unordered_set<std::string> current_sources;
  current_sources.reserve(sources.size());
  for (const auto& source : sources) {
    const auto normalized = normalized_path(source.path);
    if (!normalized.empty()) {
      current_sources.insert(normalized);
    }
  }
  remove_obsolete_source_mappings(cache, fragment_path, current_sources);

  std::unordered_set<std::string> recorded_sources;
  recorded_sources.reserve(current_sources.size());
  for (const auto& source : sources) {
    const auto normalized = normalized_path(source.path);
    if (normalized.empty() || !recorded_sources.insert(normalized).second) {
      continue;
    }
    SemanticCacheRecord record;
    record.source_path = std::filesystem::path{normalized};
    std::error_code exists_error;
    if (std::filesystem::exists(source.path, exists_error) && !exists_error) {
      record.content_hash = sha256_file_hex(source.path);
    }
    record.fragment_path = fragment_path;
    record.fragment_hash = current_fragment_hash;
    record.state = SemanticCacheState::Failed;
    record.last_error = error;
    cache.upsert(std::move(record));
  }
}

}  // namespace cgraph
