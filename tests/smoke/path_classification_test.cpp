#include "cgraph/path_classification.hpp"

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "cgraph/types.hpp"

namespace {

bool contains(const std::vector<std::string>& haystack, std::string_view needle) {
  return std::ranges::find(haystack, needle) != haystack.end();
}

void write(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_path_classification_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  // A gitignored directory, a build file, an ordinary source file, and a
  // dependency directory -- the four situations detection collapses into one.
  write(root / ".gitignore", "/research/\n");
  write(root / "research" / "evidence" / "result.json", "{}\n");
  write(root / "research" / "evidence" / "notes.txt", "x\n");
  write(root / "CMakeLists.txt", "add_test(NAME t COMMAND t)\n");
  write(root / "scripts" / "deploy.sh", "#!/bin/sh\necho hi\n");
  write(root / "src" / "lib.py", "def f():\n    return 1\n");
  write(root / "node_modules" / "dep" / "index.js", "module.exports = 1;\n");

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back(cgraph::Node{.id = "n1", .label = "f", .source_file = "src/lib.py"});

  const auto out = cgraph::classify_project_paths(root, graph);

  // The gitignored subtree is recorded by its ROOT, never enumerated: that is
  // what keeps an ignored node_modules from producing a hundred thousand rows.
  assert(contains(out.ignored_directories, "research"));
  assert(!contains(out.unindexed, "research/evidence/result.json"));
  assert(!contains(out.indexed, "research/evidence/result.json"));

  // A dependency directory is ignored the same way.
  assert(contains(out.ignored_directories, "node_modules"));

  // The load-bearing distinction: a build file and a shell script are visited
  // and unextracted, NOT ignored. A consumer that skipped work over these could
  // miss a real dependency, so they must never appear as ignored.
  assert(contains(out.unindexed, "CMakeLists.txt"));
  assert(contains(out.unindexed, "scripts/deploy.sh"));
  assert(!contains(out.ignored_files, "CMakeLists.txt"));
  assert(!contains(out.ignored_directories, "scripts"));

  // An extracted file is indexed.
  assert(contains(out.indexed, "src/lib.py"));
  assert(!contains(out.unindexed, "src/lib.py"));

  // A detected file that produced no node is unindexed, not silently indexed:
  // extraction was attempted and yielded nothing, so the graph cannot vouch for
  // it either.
  cgraph::GraphSnapshot empty_graph;
  const auto no_nodes = cgraph::classify_project_paths(root, empty_graph);
  assert(contains(no_nodes.unindexed, "src/lib.py"));

  const auto json = cgraph::path_classification_json(out);
  assert(json.at("schema_version") == 1);
  assert(json.at("paths_are") == "repo-relative");

  std::filesystem::remove_all(root);
  std::cout << "path_classification_test ok\n";
  return 0;
}
