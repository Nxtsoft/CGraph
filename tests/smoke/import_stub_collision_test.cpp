// Regression test for issues #39/#40: a TS import specifier that spells the
// source extension ("./chunkBy.ts", legal under allowImportingTsExtensions)
// used to produce module/symbol stubs whose ids equaled the real file node's
// and the real function's ids. Fragments merge in path order and "X.spec.ts"
// sorts before "X.ts", so the stubs claimed the ids first, merge_fragment
// discarded the real nodes as duplicates, and resolve_imports then deleted the
// squatting stubs (no file node matched their path) — erasing the file, its
// function, and every edge. On es-toolkit this deleted 650 of 1508 files.
#include "cgraph/pipeline.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_import_stub_collision_test";
  std::filesystem::remove_all(root);

  // The impl and a spec importing it WITH the .ts extension. The spec's path
  // sorts before the impl's, reproducing the merge order that triggered the bug.
  write_file(root / "chunkBy.ts",
             "export function chunkBy(arr: number[]): number[][] {\n  return [arr];\n}\n");
  write_file(root / "chunkBy.spec.ts",
             "import { chunkBy } from './chunkBy.ts';\n\nexport const result = chunkBy([1]);\n");
  // Control: an extension-less import must keep resolving as before.
  write_file(root / "user.ts",
             "import { chunkBy } from './chunkBy';\n\nexport const used = chunkBy([2]);\n");

  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;

  // File labels carry the parent directory ("dir/chunkBy.ts"), so match on suffix.
  const auto node_with = [&](std::string_view kind, std::string_view label_suffix) {
    return std::ranges::any_of(graph.nodes, [&](const auto& node) {
      return node.kind == kind && std::string_view(node.label).ends_with(label_suffix);
    });
  };
  const auto edge = [&](std::string_view relation, std::string_view target_suffix) {
    return std::ranges::any_of(graph.edges, [&](const auto& e) {
      return e.relation == relation && e.target.ends_with(target_suffix);
    });
  };

  std::filesystem::remove_all(root);

  // The imported file and its function must survive extraction.
  if (!node_with("file", "chunkBy.ts")) {
    return 1;
  }
  if (!node_with("function", "chunkBy")) {
    return 2;
  }
  // No unresolved stub may remain.
  if (std::ranges::any_of(graph.nodes,
                          [](const auto& n) { return n.kind == "module" || n.kind == "import"; })) {
    return 3;
  }
  // Both the extension-spelled and the extension-less import must resolve to
  // real nodes: imports_from onto the file, imports onto the declared symbol.
  if (!edge("imports_from", "chunkby_ts")) {
    return 4;
  }
  if (!edge("imports", "chunkby_ts_chunkby")) {
    return 5;
  }
  return 0;
}
