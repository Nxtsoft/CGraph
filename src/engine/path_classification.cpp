#include "cgraph/path_classification.hpp"

#include <algorithm>
#include <set>
#include <system_error>

#include <nlohmann/json.hpp>

#include "cgraph/detect.hpp"
#include "cgraph/path_ignore.hpp"

namespace cgraph {
namespace {

std::string relative_to(const std::filesystem::path& root, const std::filesystem::path& path) {
  return path.lexically_relative(root).generic_string();
}

}  // namespace

PathClassification classify_project_paths(const std::filesystem::path& root,
                                          const GraphSnapshot& graph) {
  PathClassification out;
  const auto canonical_root = std::filesystem::weakly_canonical(root);
  const auto gitignore_patterns = read_root_gitignore(canonical_root);

  // The indexed set comes from the graph rather than from re-extraction: a file
  // detection accepted but extraction produced nothing for is unindexed, and
  // only the graph knows that.
  std::set<std::string> indexed;
  for (const auto& node : graph.nodes) {
    if (node.source_file.empty()) continue;
    const std::filesystem::path source(node.source_file);
    indexed.insert(source.is_absolute() ? relative_to(canonical_root, source)
                                        : std::filesystem::path(node.source_file).generic_string());
  }

  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      canonical_root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;

  std::set<std::string> unindexed;
  std::set<std::string> ignored_dirs;
  std::set<std::string> ignored_files;

  for (; !error && iterator != end; iterator.increment(error)) {
    const auto& entry = *iterator;

    if (entry.is_directory(error)) {
      // Mirrors detect_project_files: a dependency or gitignored directory is
      // not descended into, so its contents are never visited. Record the root;
      // the consumer treats everything beneath it as deliberately ignored.
      if (is_dependency_directory(entry.path()) ||
          matches_simple_gitignore(canonical_root, entry.path(), gitignore_patterns)) {
        iterator.disable_recursion_pending();
        ignored_dirs.insert(relative_to(canonical_root, entry.path()));
      }
      continue;
    }

    if (!entry.is_regular_file(error)) {
      continue;
    }

    const auto relative = relative_to(canonical_root, entry.path());

    if (matches_simple_gitignore(canonical_root, entry.path(), gitignore_patterns)) {
      ignored_files.insert(relative);
      continue;
    }

    // Detection accepted the file only if a language claimed it. Anything else
    // is visited-but-unextracted, which is the verdict a consumer must not
    // mistake for "irrelevant".
    if (detect_language(entry.path()) == DetectedLanguage::Unknown) {
      unindexed.insert(relative);
      continue;
    }
    // A detected file that produced no node is also unindexed: extraction was
    // attempted and yielded nothing, so the graph cannot vouch for it either.
    if (!indexed.contains(relative)) {
      unindexed.insert(relative);
    }
  }

  out.indexed.assign(indexed.begin(), indexed.end());
  out.unindexed.assign(unindexed.begin(), unindexed.end());
  out.ignored_directories.assign(ignored_dirs.begin(), ignored_dirs.end());
  out.ignored_files.assign(ignored_files.begin(), ignored_files.end());
  return out;
}

nlohmann::json path_classification_json(const PathClassification& classification) {
  return nlohmann::json{
      {"schema_version", 1},
      {"paths_are", "repo-relative"},
      {"indexed", classification.indexed},
      {"unindexed", classification.unindexed},
      {"ignored_directories", classification.ignored_directories},
      {"ignored_files", classification.ignored_files},
  };
}

}  // namespace cgraph
