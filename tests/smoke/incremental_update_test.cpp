#include "cgraph/incremental_update.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/pipeline.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

bool has_node_label(const cgraph::GraphSnapshot& graph, const std::string& label) {
  for (const auto& node : graph.nodes) {
    if (node.label == label) {
      return true;
    }
  }
  return false;
}

bool has_source(const cgraph::GraphSnapshot& graph, const std::filesystem::path& path) {
  const auto expected = path.generic_string();
  for (const auto& node : graph.nodes) {
    if (node.source_file == expected) {
      return true;
    }
  }
  return false;
}

bool has_source_hash(
    const cgraph::GraphSnapshot& graph,
    const std::filesystem::path& path,
    std::string_view exact_source) {
  const auto key = path.lexically_normal().generic_string();
  const auto found = graph.source_hashes.find(key);
  return found != graph.source_hashes.end() && found->second == cgraph::sha256_hex(exact_source);
}

std::unordered_set<std::string> node_labels(const cgraph::GraphSnapshot& graph) {
  std::unordered_set<std::string> labels;
  for (const auto& node : graph.nodes) {
    labels.insert(node.label);
  }
  return labels;
}

cgraph::ContentRoot expected_content_root(const cgraph::IncrementalGraphIndex& index) {
  std::vector<cgraph::FileCacheEntry> entries;
  entries.reserve(index.cache.size());
  for (const auto& [_, entry] : index.cache) {
    entries.push_back(entry);
  }
  return cgraph::compute_content_root(index.project_root, entries);
}

bool same_content_root(const cgraph::ContentRoot& lhs, const cgraph::ContentRoot& rhs) {
  return lhs.algorithm == rhs.algorithm && lhs.sha256 == rhs.sha256 && lhs.leaf_count == rhs.leaf_count;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph-incremental-update-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto alpha = root / "alpha.py";
  const auto beta = root / "beta.py";
  const auto renamed = root / "renamed.py";

  cgraph::DaemonState state;
  cgraph::IncrementalGraphIndex index;
  index.project_root = root;

  const std::string alpha_source = "class Alpha:\n    pass\n";
  const std::string beta_source = "class Beta:\n    pass\n";
  write_file(alpha, alpha_source);
  write_file(beta, beta_source);
  cgraph::FileWatchEvent created[] = {
      {.path = alpha, .change = cgraph::FileWatchChange::Created, .kind = cgraph::WatchedFileKind::Code},
      {.path = beta, .change = cgraph::FileWatchChange::Created, .kind = cgraph::WatchedFileKind::Code},
  };
  auto result = cgraph::apply_incremental_code_updates(state, index, created);
  auto graph = cgraph::read_graph_snapshot(state);
  if (result.files_hashed != 2 || result.bytes_hashed == 0 || result.files_reextracted != 2 ||
      !has_node_label(*graph, "Alpha") || !has_node_label(*graph, "Beta") ||
      graph->content_root.algorithm != "sha256-merkle-v1" || graph->content_root.sha256.size() != 64 ||
      graph->content_root.leaf_count != 2 ||
      graph->source_hashes.size() != 2 || !has_source_hash(*graph, alpha, alpha_source) ||
      !has_source_hash(*graph, beta, beta_source) ||
      !same_content_root(graph->content_root, expected_content_root(index))) {
    return 1;
  }
  const auto created_root = graph->content_root;

#if defined(__APPLE__) || defined(__unix__)
  // A source that becomes unreadable after a verified publish must not replace
  // either the live snapshot or its incremental index with unverified state.
  const auto verified_snapshot = graph;
  const auto verified_project_root = index.project_root;
  const auto verified_files_size = index.files.size();
  const auto verified_cache_size = index.cache.size();
  const auto alpha_key = cgraph::incremental_file_key(alpha);
  const auto verified_alpha_source_sha256 = index.files.at(alpha_key).source_sha256;
  const auto verified_alpha_cache_sha256 = index.cache.at(alpha_key).sha256;

  std::error_code permission_error;
  std::filesystem::permissions(
      alpha,
      std::filesystem::perms::none,
      std::filesystem::perm_options::replace,
      permission_error);
  if (permission_error || !cgraph::sha256_file_hex(alpha).empty()) {
    return 1;
  }

  cgraph::IncrementalUpdateResult failed_rescan;
  cgraph::IncrementalUpdateResult failed_incremental;
  try {
    failed_rescan = cgraph::full_stat_index_rescan(state, index, root);
    cgraph::FileWatchEvent unreadable_event[] = {
        {.path = alpha,
         .change = cgraph::FileWatchChange::Modified,
         .kind = cgraph::WatchedFileKind::Code},
    };
    failed_incremental =
        cgraph::apply_incremental_code_updates(state, index, unreadable_event);
  } catch (...) {
    std::filesystem::permissions(
        alpha,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        permission_error);
    throw;
  }
  std::filesystem::permissions(
      alpha,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      permission_error);
  graph = cgraph::read_graph_snapshot(state);
  if (permission_error || graph != verified_snapshot ||
      index.project_root != verified_project_root ||
      index.files.size() != verified_files_size ||
      index.cache.size() != verified_cache_size ||
      index.files.at(alpha_key).source_sha256 != verified_alpha_source_sha256 ||
      index.cache.at(alpha_key).sha256 != verified_alpha_cache_sha256 ||
      failed_rescan.warnings.empty() || failed_incremental.warnings.empty()) {
    return 1;
  }
#endif

  const auto alpha_modified_at = std::filesystem::last_write_time(alpha);
  std::filesystem::last_write_time(alpha, alpha_modified_at + std::chrono::seconds(5));
  cgraph::FileWatchEvent touched[] = {
      {.path = alpha, .change = cgraph::FileWatchChange::Modified, .kind = cgraph::WatchedFileKind::Code},
  };
  result = cgraph::apply_incremental_code_updates(state, index, touched);
  graph = cgraph::read_graph_snapshot(state);
  if (result.files_hashed != 1 || result.bytes_hashed == 0 || result.files_reextracted != 0 ||
      result.files_cache_hit != 1 || result.files_removed != 0 ||
      result.neighborhood_deduped || !has_node_label(*graph, "Alpha") || !has_node_label(*graph, "Beta") ||
      graph->content_root.sha256 != created_root.sha256 ||
      graph->source_hashes.size() != 2 || !has_source_hash(*graph, alpha, alpha_source) ||
      !has_source_hash(*graph, beta, beta_source) ||
      !same_content_root(graph->content_root, expected_content_root(index))) {
    return 1;
  }

  const auto preserved_modified_at = std::filesystem::last_write_time(alpha);
  const std::string gamma_source = "class Gamma:\n    pass\n";
  write_file(alpha, gamma_source);
  std::filesystem::last_write_time(alpha, preserved_modified_at);
  cgraph::FileWatchEvent edited[] = {
      {.path = alpha, .change = cgraph::FileWatchChange::Modified, .kind = cgraph::WatchedFileKind::Code},
  };
  result = cgraph::apply_incremental_code_updates(state, index, edited);
  graph = cgraph::read_graph_snapshot(state);
  if (result.files_hashed != 1 || result.bytes_hashed == 0 || result.files_reextracted != 1 ||
      has_node_label(*graph, "Alpha") || !has_node_label(*graph, "Gamma") ||
      graph->content_root.sha256 == created_root.sha256 || graph->content_root.leaf_count != 2 ||
      graph->source_hashes.size() != 2 || !has_source_hash(*graph, alpha, gamma_source) ||
      !has_source_hash(*graph, beta, beta_source) ||
      !same_content_root(graph->content_root, expected_content_root(index))) {
    return 1;
  }
  const auto edited_root = graph->content_root;

  std::filesystem::rename(beta, renamed);
  cgraph::FileWatchEvent rename_events[] = {
      {.path = beta, .change = cgraph::FileWatchChange::Deleted, .kind = cgraph::WatchedFileKind::Code},
      {.path = renamed, .change = cgraph::FileWatchChange::Created, .kind = cgraph::WatchedFileKind::Code},
  };
  result = cgraph::apply_incremental_code_updates(state, index, rename_events);
  graph = cgraph::read_graph_snapshot(state);
  if (result.files_hashed != 1 || result.bytes_hashed == 0 || result.files_removed != 1 ||
      result.files_reextracted != 1 || has_source(*graph, beta) ||
      !has_source(*graph, renamed) || graph->content_root.sha256 == edited_root.sha256 ||
      graph->content_root.leaf_count != 2 ||
      graph->source_hashes.size() != 2 || graph->source_hashes.contains(beta.lexically_normal().generic_string()) ||
      !has_source_hash(*graph, alpha, gamma_source) || !has_source_hash(*graph, renamed, beta_source) ||
      !same_content_root(graph->content_root, expected_content_root(index))) {
    return 1;
  }
  const auto renamed_root = graph->content_root;

  std::filesystem::remove(renamed);
  cgraph::FileWatchEvent deleted[] = {
      {.path = renamed, .change = cgraph::FileWatchChange::Deleted, .kind = cgraph::WatchedFileKind::Code},
  };
  result = cgraph::apply_incremental_code_updates(state, index, deleted);
  graph = cgraph::read_graph_snapshot(state);
  if (result.files_hashed != 0 || result.bytes_hashed != 0 || result.files_removed != 1 ||
      has_node_label(*graph, "Beta") || has_source(*graph, renamed) ||
      graph->content_root.sha256 == renamed_root.sha256 || graph->content_root.leaf_count != 1 ||
      graph->source_hashes.size() != 1 || !has_source_hash(*graph, alpha, gamma_source) ||
      graph->source_hashes.contains(renamed.lexically_normal().generic_string()) ||
      !same_content_root(graph->content_root, expected_content_root(index))) {
    return 1;
  }

  // A full content-verified rescan publishes the same exact extraction hashes:
  // unchanged files retain their parsed-buffer evidence and newly detected files
  // receive evidence from the exact buffer handed to the extractor.
  const std::string delta_source = "class Delta:\n    pass\n";
  write_file(beta, delta_source);
  result = cgraph::full_stat_index_rescan(state, index, root);
  graph = cgraph::read_graph_snapshot(state);
  if (!result.full_rescan || !result.full_dedup_reconciled ||
      graph->source_hashes.size() != 2 ||
      !has_source_hash(*graph, std::filesystem::weakly_canonical(alpha), gamma_source) ||
      !has_source_hash(*graph, std::filesystem::weakly_canonical(beta), delta_source) ||
      !same_content_root(graph->content_root, expected_content_root(index))) {
    return 1;
  }

  // The daemon's rescan must produce the same graph as the canonical one-shot
  // pipeline: identical labels AND identical node/edge counts. The count check
  // guards the pipeline ordering — the daemon must dedup before community
  // detection just like run_one_shot, or community-bucketed dedup over-merges
  // and the counts diverge.
  const auto full_build = cgraph::run_one_shot(root);
  graph = cgraph::read_graph_snapshot(state);
  if (node_labels(*graph) != node_labels(full_build.graph)) {
    return 1;
  }
  if (graph->nodes.size() != full_build.graph.nodes.size() ||
      graph->edges.size() != full_build.graph.edges.size()) {
    return 1;
  }
  // The daemon graph carries community + centrality (computed after dedup), so
  // ranked queries work on the resident snapshot.
  bool has_centrality = false;
  for (const auto& node : graph->nodes) {
    if (node.properties.contains("degree_centrality")) {
      has_centrality = true;
      break;
    }
  }
  if (!has_centrality) {
    return 1;
  }

  std::filesystem::remove_all(root);
  return 0;
}
