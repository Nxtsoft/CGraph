// One-shot builds now carry the sha256-merkle-v1 content root and graph.json
// round-trips it: same tree -> same root, edited tree -> different root, and
// the exported metadata parses back to an identical, valid ContentRoot. This
// is the provenance surface CI test selection pins against instead of mtimes.
#include "cgraph/export_json.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/pipeline.hpp"

#include <filesystem>
#include <fstream>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_content_root_export_test";
  std::filesystem::remove_all(root);
  write_file(root / "a.py", "def alpha():\n    return 1\n");
  write_file(root / "b.py", "def beta():\n    return 2\n");

  const auto first = cgraph::run_one_shot(root);
  if (!cgraph::is_valid_content_root(first.graph.content_root)) {
    std::filesystem::remove_all(root);
    return 1;
  }

  // Determinism: an identical tree yields the identical root.
  const auto second = cgraph::run_one_shot(root);
  if (second.graph.content_root.sha256 != first.graph.content_root.sha256 ||
      second.graph.content_root.leaf_count != first.graph.content_root.leaf_count) {
    std::filesystem::remove_all(root);
    return 2;
  }

  // graph.json round-trip: the exported metadata parses back byte-identical.
  const auto exported = cgraph::to_node_link_json(first.graph);
  const auto parsed = cgraph::parse_node_link_graph(exported);
  if (!cgraph::is_valid_content_root(parsed.content_root) ||
      parsed.content_root.sha256 != first.graph.content_root.sha256 ||
      parsed.content_root.leaf_count != first.graph.content_root.leaf_count) {
    std::filesystem::remove_all(root);
    return 3;
  }

  // Sensitivity: editing one file changes the root.
  write_file(root / "a.py", "def alpha():\n    return 42\n");
  const auto third = cgraph::run_one_shot(root);
  std::filesystem::remove_all(root);
  if (!cgraph::is_valid_content_root(third.graph.content_root) ||
      third.graph.content_root.sha256 == first.graph.content_root.sha256) {
    return 4;
  }
  return 0;
}
