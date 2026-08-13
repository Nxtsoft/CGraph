#include "cgraph/semantic_fragment_validation.hpp"

#include "cgraph/file_cache.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph-semantic-fragment-validation-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto valid_path = root / "chunk_01.json";
  const std::string valid_contents = R"({
        "nodes": [
          {"id": "doc:one", "label": "Doc One", "type": "document", "confidence": "INFERRED"}
        ],
        "edges": [
          {"source": "doc:one", "target": "symbol:two", "relation": "MENTIONS", "confidence": "INFERRED"}
        ],
        "hyperedges": []
      })";
  write_file(valid_path, valid_contents);

  const auto valid = cgraph::validate_semantic_fragment_file(valid_path);
  if (!valid.valid || valid.fragment.nodes.size() != 1 || valid.fragment.edges.size() != 1 || !valid.errors.empty() ||
      valid.source_sha256 != cgraph::sha256_hex(valid_contents)) {
    return 1;
  }

  const auto malformed_path = root / "chunk_02.json";
  const std::string malformed_contents = R"({"nodes": [)";
  write_file(malformed_path, malformed_contents);
  const auto malformed = cgraph::validate_semantic_fragment_file(malformed_path);
  if (malformed.valid || malformed.errors.empty() || !malformed.fragment.nodes.empty() ||
      malformed.source_sha256 != cgraph::sha256_hex(malformed_contents)) {
    return 1;
  }

  const auto invalid_schema_path = root / "chunk_03.json";
  write_file(invalid_schema_path, R"({"nodes":[{"id":"missing-label"}],"edges":[{"source":"a","target":"b"}]})");
  const auto invalid_schema = cgraph::validate_semantic_fragment_file(invalid_schema_path);
  if (invalid_schema.valid || invalid_schema.errors.empty() || invalid_schema.fragment.nodes.empty()) {
    return 1;
  }

  std::filesystem::remove_all(root);
  return 0;
}
