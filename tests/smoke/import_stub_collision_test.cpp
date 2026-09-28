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

  // A root-level relative import must bind to the file it names even when
  // another file shares its basename. resolve_imports finds the file by its
  // absolute path; an import_path made project-relative ("util") would fall
  // through to suffix matching, tie between util.ts and lib/util.ts, and drop
  // the import and the imports edge with it.
  const auto dup_root = std::filesystem::temp_directory_path() / "cgraph_import_basename_test";
  std::filesystem::remove_all(dup_root);
  write_file(dup_root / "util.ts", "export function util(): number {\n  return 1;\n}\n");
  write_file(dup_root / "lib" / "util.ts", "export function util(): number {\n  return 2;\n}\n");
  write_file(dup_root / "index.ts", "import { util } from './util';\n\nexport const value = util();\n");
  const auto dup = cgraph::run_one_shot(dup_root);
  // Detected paths are canonical (on macOS /var -> /private/var), so compare
  // against the canonical root, not the raw temp path.
  const auto root_util = (std::filesystem::weakly_canonical(dup_root) / "util.ts").lexically_normal().generic_string();
  std::string root_util_file;
  std::string root_util_fn;
  std::string index_file;
  for (const auto& node : dup.graph.nodes) {
    const auto file = std::filesystem::path(node.source_file).lexically_normal().generic_string();
    if (file == root_util && node.kind == "file") root_util_file = node.id;
    if (file == root_util && node.kind == "function" && node.label == "util") root_util_fn = node.id;
    if (node.kind == "file" && std::string_view(node.label).ends_with("index.ts")) index_file = node.id;
  }
  const auto dup_edge = [&](std::string_view relation, const std::string& target) {
    return std::ranges::any_of(dup.graph.edges, [&](const auto& e) {
      return e.relation == relation && e.source == index_file && e.target == target;
    });
  };
  std::filesystem::remove_all(dup_root);
  if (root_util_file.empty() || root_util_fn.empty() || index_file.empty()) {
    return 6;
  }
  if (!dup_edge("imports_from", root_util_file)) {
    return 7;
  }
  if (!dup_edge("imports", root_util_fn)) {
    return 8;
  }
  return 0;
}
