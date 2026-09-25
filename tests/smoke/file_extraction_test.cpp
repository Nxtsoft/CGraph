#include "cgraph/file_extraction.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

// Compares two extraction results by their observable fragment shape: node ids
// and labels in order, plus edge endpoints. Enough to prove a parallel result
// is identical to the serial one for the same file.
[[nodiscard]] bool same_fragment(const cgraph::ExtractionResult& lhs, const cgraph::ExtractionResult& rhs) {
  if (lhs.source_sha256 != rhs.source_sha256) return false;
  if (lhs.fragment.nodes.size() != rhs.fragment.nodes.size()) return false;
  if (lhs.fragment.edges.size() != rhs.fragment.edges.size()) return false;
  if (lhs.fragment.warnings != rhs.fragment.warnings) return false;
  for (std::size_t i = 0; i < lhs.fragment.nodes.size(); ++i) {
    if (lhs.fragment.nodes[i].id != rhs.fragment.nodes[i].id) return false;
    if (lhs.fragment.nodes[i].label != rhs.fragment.nodes[i].label) return false;
  }
  for (std::size_t i = 0; i < lhs.fragment.edges.size(); ++i) {
    if (lhs.fragment.edges[i].source != rhs.fragment.edges[i].source) return false;
    if (lhs.fragment.edges[i].target != rhs.fragment.edges[i].target) return false;
    if (lhs.fragment.edges[i].relation != rhs.fragment.edges[i].relation) return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_file_extraction_test";
  std::filesystem::remove_all(root);
  const auto source_path = root / "main.py";
  constexpr const char* source = "def main():\n    return helper()\n";
  write_file(source_path, source);

  const auto result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = source_path,
      .language = cgraph::DetectedLanguage::Python,
  }, root);
  if (result.fragment.nodes.empty() || !result.fragment.warnings.empty() ||
      result.source_sha256 != cgraph::sha256_hex(source)) {
    std::filesystem::remove_all(root);
    return 1;
  }

  const auto missing = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = root / "missing.py",
      .language = cgraph::DetectedLanguage::Python,
  }, root);
  if (!missing.fragment.nodes.empty() || missing.fragment.warnings.empty() || !missing.source_sha256.empty()) {
    std::filesystem::remove_all(root);
    return 1;
  }

  // Parallel extraction must produce, in detection order, results identical to
  // serial extraction file-by-file. Build a batch large enough to keep multiple
  // worker threads contended, mixing languages and one missing file.
  std::vector<cgraph::DetectedFile> batch;
  for (int i = 0; i < 64; ++i) {
    const auto py = root / ("mod" + std::to_string(i) + ".py");
    write_file(py, "def f():\n    return g()\n\ndef g():\n    return 1\n");
    batch.push_back({.path = py, .language = cgraph::DetectedLanguage::Python});

    const auto ts = root / ("mod" + std::to_string(i) + ".ts");
    write_file(ts, "export function f() { return g(); }\nfunction g() { return 1; }\n");
    batch.push_back({.path = ts, .language = cgraph::DetectedLanguage::TypeScript});
  }
  batch.push_back({.path = root / "gone.py", .language = cgraph::DetectedLanguage::Python});

  const auto parallel = cgraph::extract_files(batch, root);
  if (parallel.size() != batch.size()) {
    std::filesystem::remove_all(root);
    return 1;
  }
  for (std::size_t i = 0; i < batch.size(); ++i) {
    const auto serial = cgraph::extract_detected_file(batch[i], root);
    if (!same_fragment(parallel[i], serial)) {
      std::filesystem::remove_all(root);
      return 1;
    }
  }

  // An empty batch is valid and yields no results.
  if (!cgraph::extract_files({}, root).empty()) {
    std::filesystem::remove_all(root);
    return 1;
  }

  // A file outside the project root has no project-relative path, so it is an
  // extraction failure (warning, no nodes) rather than an id that climbs above
  // the root.
  const auto outside_root = std::filesystem::temp_directory_path() / "cgraph_file_extraction_outside";
  std::filesystem::remove_all(outside_root);
  write_file(outside_root / "stray.py", "def stray():\n    return 1\n");
  const auto stray = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = outside_root / "stray.py",
      .language = cgraph::DetectedLanguage::Python,
  }, root);
  if (!stray.fragment.nodes.empty() || stray.fragment.warnings.empty() ||
      stray.fragment.warnings.front().find("outside the project root") == std::string::npos) {
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside_root);
    return 2;
  }
  std::filesystem::remove_all(outside_root);
  std::filesystem::remove_all(root);

  // Node ids derive from the project-relative path: the same tree extracted from
  // two absolute roots of different depth yields byte-identical ids, and no id
  // carries a path segment above the root. A file at the root, a nested file and
  // a non-ASCII path each normalize to exactly make_id(relative path).
  const auto shallow = std::filesystem::temp_directory_path() / "cgraph_ids_shallow";
  const auto deep = std::filesystem::temp_directory_path() / "cgraph_ids_deep" / "nested" / "checkout" / "depth";
  const auto extract_ids = [&](const std::filesystem::path& project_root) {
    std::filesystem::remove_all(project_root);
    write_file(project_root / "main.py", "from pkg.service import Service\n\ndef main():\n    return Service().run()\n");
    write_file(project_root / "pkg" / "service.py", "class Service:\n    def run(self):\n        return helper()\n\ndef helper():\n    return 1\n");
    write_file(project_root / "pkg" / "util.ts", "export function util() { return 1; }\n");
    write_file(project_root / "données" / "résumé.py", "def résumé():\n    return 1\n");
    const auto detected = cgraph::detect_project_files(project_root);
    std::vector<std::string> ids;
    for (const auto& extraction : cgraph::extract_files(detected, std::filesystem::weakly_canonical(project_root))) {
      for (const auto& node : extraction.fragment.nodes) {
        ids.push_back(node.id);
      }
      for (const auto& edge : extraction.fragment.edges) {
        ids.push_back(edge.source + " -" + edge.relation + "-> " + edge.target);
      }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  };
  const auto shallow_ids = extract_ids(shallow);
  const auto deep_ids = extract_ids(deep);
  const auto cleanup = [&] {
    std::filesystem::remove_all(shallow);
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "cgraph_ids_deep");
  };
  if (shallow_ids.size() < 8 || shallow_ids != deep_ids) {
    cleanup();
    return 3;
  }
  const auto has_id = [&](const std::string& id) {
    return std::find(shallow_ids.begin(), shallow_ids.end(), id) != shallow_ids.end();
  };
  for (const auto& id : shallow_ids) {
    if (id.find("cgraph_ids") != std::string::npos || id.find("nested_checkout_depth") != std::string::npos) {
      cleanup();
      return 4;  // a segment above the project root leaked into an id
    }
  }
  if (!has_id(cgraph::make_id("main.py")) || !has_id(cgraph::make_id("pkg/service.py")) ||
      !has_id(cgraph::make_id("pkg/service.py:Service")) || !has_id(cgraph::make_id("données/résumé.py")) ||
      !has_id(cgraph::make_id("données/résumé.py:résumé"))) {
    cleanup();
    return 5;
  }
  cleanup();
  return 0;
}
