#include "cgraph/index_persistence.hpp"

#include "cgraph/file_cache.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "cgraph_index_persistence_test";
  fs::remove_all(root);

  const auto a = root / "a.py";
  const auto b = root / "src" / "b.ts";
  write_file(a, "def f():\n    return 1\n");
  write_file(b, "export const x = 1;\n");

  // The version key is non-empty, carries the format prefix, and is stable.
  const auto key = cgraph::index_version_key();
  if (key.empty() || key.rfind("cgraph-index-v1:", 0) != 0) {
    fs::remove_all(root);
    return 1;
  }
  if (cgraph::index_version_key() != key) {
    fs::remove_all(root);
    return 1;
  }

  // Regression guard: the key is a fixed logic version, NOT a hash of the running
  // binary. A binary-hash key invalidated every project's fast-load cache on any
  // rebuild or re-sign, forcing a cold rebuild on the next query. The trailing
  // segment must therefore stay a short literal (bumped by hand on parity-surface
  // changes) and never a 64-char hex digest. Update this literal when you bump it.
  if (key != "cgraph-index-v1:logic-4") {
    fs::remove_all(root);
    return 1;
  }

  cgraph::IndexManifest manifest;
  manifest.version_key = key;
  manifest.files.push_back(cgraph::read_file_cache_entry(a));
  manifest.files.push_back(cgraph::read_file_cache_entry(b));
  manifest.content_root = cgraph::compute_content_root(root, manifest.files);

  // Round-trip: write then read yields an equal manifest.
  const auto manifest_path = root / "cgraph-out" / "index-manifest.json";
  auto invalid_manifest = manifest;
  invalid_manifest.content_root.sha256.clear();
  if (cgraph::write_index_manifest(invalid_manifest, root / "invalid-manifest.json")) {
    fs::remove_all(root);
    return 1;
  }
  if (!cgraph::write_index_manifest(manifest, manifest_path)) {
    fs::remove_all(root);
    return 1;
  }
  const auto loaded = cgraph::read_index_manifest(manifest_path);
  if (!loaded || loaded->version_key != key || loaded->files.size() != 2 ||
      loaded->content_root.algorithm != manifest.content_root.algorithm ||
      loaded->content_root.sha256 != manifest.content_root.sha256 ||
      loaded->content_root.leaf_count != manifest.content_root.leaf_count) {
    fs::remove_all(root);
    return 1;
  }
  for (std::size_t i = 0; i < manifest.files.size(); ++i) {
    const auto& lhs = manifest.files[i];
    const auto& rhs = loaded->files[i];
    if (lhs.path != rhs.path || lhs.size != rhs.size || lhs.modified_at != rhs.modified_at ||
        lhs.sha256 != rhs.sha256) {
      fs::remove_all(root);
      return 1;  // a field did not survive the round-trip (mtime is the risky one)
    }
  }

  const std::vector<cgraph::DetectedFile> unchanged = {
      {.path = a, .language = cgraph::DetectedLanguage::Python},
      {.path = b, .language = cgraph::DetectedLanguage::TypeScript},
  };

  // Unchanged tree matches.
  if (!cgraph::tree_matches_manifest(*loaded, unchanged, root)) {
    fs::remove_all(root);
    return 1;
  }
  // The aggregate source identity is independently gated: a mismatched
  // algorithm, digest, or leaf count rejects fast-load even when every
  // individual file hash still matches.
  auto wrong_algorithm = *loaded;
  wrong_algorithm.content_root.algorithm = "sha256-merkle-v0";
  if (cgraph::tree_matches_manifest(wrong_algorithm, unchanged, root)) {
    fs::remove_all(root);
    return 1;
  }
  auto wrong_digest = *loaded;
  wrong_digest.content_root.sha256[0] = wrong_digest.content_root.sha256[0] == '0' ? '1' : '0';
  if (cgraph::tree_matches_manifest(wrong_digest, unchanged, root)) {
    fs::remove_all(root);
    return 1;
  }
  auto wrong_leaf_count = *loaded;
  ++wrong_leaf_count.content_root.leaf_count;
  if (cgraph::tree_matches_manifest(wrong_leaf_count, unchanged, root)) {
    fs::remove_all(root);
    return 1;
  }

  // A removed file (manifest has 2, only 1 detected) does not match.
  const std::vector<cgraph::DetectedFile> removed = {
      {.path = a, .language = cgraph::DetectedLanguage::Python},
  };
  if (cgraph::tree_matches_manifest(*loaded, removed, root)) {
    fs::remove_all(root);
    return 1;
  }

  // An added file (3 detected vs 2 in manifest) does not match.
  const auto c = root / "c.py";
  write_file(c, "x = 2\n");
  std::vector<cgraph::DetectedFile> added = unchanged;
  added.push_back({.path = c, .language = cgraph::DetectedLanguage::Python});
  if (cgraph::tree_matches_manifest(*loaded, added, root)) {
    fs::remove_all(root);
    return 1;
  }

  // A same-length rewrite with the original mtime restored still does not
  // match: startup validation must hash every detected file.
  const auto preserved_modified_at = fs::last_write_time(a);
  write_file(a, "def g():\n    return 2\n");
  fs::last_write_time(a, preserved_modified_at);
  if (cgraph::tree_matches_manifest(*loaded, unchanged, root)) {
    fs::remove_all(root);
    return 1;
  }

  // A pre-root manifest is unusable rather than silently upgraded.
  write_file(root / "missing-root.json", "{\"version_key\":\"cgraph-index-v1:logic-2\",\"files\":[]}");
  if (cgraph::read_index_manifest(root / "missing-root.json").has_value()) {
    fs::remove_all(root);
    return 1;
  }

  // A missing or corrupt manifest is "no usable cache", not a crash.
  if (cgraph::read_index_manifest(root / "nope.json").has_value()) {
    fs::remove_all(root);
    return 1;
  }
  write_file(root / "bad.json", "{ this is not json");
  if (cgraph::read_index_manifest(root / "bad.json").has_value()) {
    fs::remove_all(root);
    return 1;
  }

  fs::remove_all(root);
  return 0;
}
