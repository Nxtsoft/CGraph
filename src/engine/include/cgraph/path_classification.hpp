#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "cgraph/types.hpp"

namespace cgraph {

// Why a path is absent from the graph. Detection drops three different
// situations with the same bare `continue` (detect.cpp), so a consumer asking
// "is this file in the graph?" gets one answer -- no -- for reasons that are not
// interchangeable:
//
//   ignored    deliberately skipped: a root .gitignore match or a dependency
//              directory. The graph is complete WITHOUT it, so a consumer may
//              treat it as irrelevant.
//   unindexed  visited, not ignored, but no extractor claimed it. The graph may
//              be INCOMPLETE because of it. `CMakeLists.txt`, `.sh`, `.cmake`
//              and `.yaml` land here, and those change what tests mean, so a
//              consumer MUST NOT treat these as irrelevant.
//
// Collapsing the two is worse than reporting neither: it would let a consumer
// skip work over a build file it could not see.
struct PathClassification {
  // Repo-relative, generic separators, sorted. Files that produced graph nodes.
  std::vector<std::string> indexed;
  // Repo-relative, sorted. Visited and not ignored, but unextracted. NOT safe
  // for a consumer to treat as irrelevant.
  std::vector<std::string> unindexed;
  // Repo-relative subtree roots whose contents were never walked. Recording the
  // root rather than its contents keeps this bounded: an ignored `node_modules`
  // is one entry, not a hundred thousand.
  std::vector<std::string> ignored_directories;
  // Repo-relative files matching the root .gitignore directly.
  std::vector<std::string> ignored_files;
};

// Walks `root` exactly as detect_project_files does -- same predicates, same
// order -- but records the verdict at each skip point instead of discarding it.
// `graph` supplies the indexed set.
[[nodiscard]] PathClassification classify_project_paths(const std::filesystem::path& root,
                                                        const GraphSnapshot& graph);

[[nodiscard]] nlohmann::json path_classification_json(const PathClassification& classification);

}  // namespace cgraph
