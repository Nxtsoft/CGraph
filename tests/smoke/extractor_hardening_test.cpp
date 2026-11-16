#include "cgraph/file_extraction.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

namespace {

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

bool contains_warning(const cgraph::ExtractionResult& result, std::string_view needle) {
  for (const auto& warning : result.fragment.warnings) {
    if (warning.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool distinct_non_empty_ids(const cgraph::ExtractionResult& result) {
  std::set<std::string> ids;
  for (const auto& node : result.fragment.nodes) {
    if (node.id.empty() || !ids.insert(node.id).second) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph-extractor-hardening-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto malformed = root / "mcp.json";
  write_file(malformed, R"({"mcpServers":{"broken":)");
  const auto malformed_result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = malformed,
      .language = cgraph::DetectedLanguage::McpConfig,
  }, root);
  if (!malformed_result.fragment.nodes.empty() || !contains_warning(malformed_result, "failed to parse MCP config JSON")) {
    std::filesystem::remove_all(root);
    return 1;
  }

  std::string invalid_utf8 = "def bad_utf8():\n    return '";
  invalid_utf8.push_back(static_cast<char>(0xC3));
  invalid_utf8.push_back(static_cast<char>(0x28));
  invalid_utf8 += "'\n";
  const auto invalid_utf8_path = root / "bad_utf8.py";
  write_file(invalid_utf8_path, invalid_utf8);
  const auto invalid_utf8_result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = invalid_utf8_path,
      .language = cgraph::DetectedLanguage::Python,
  }, root);
  if (invalid_utf8_result.fragment.nodes.empty() || !invalid_utf8_result.fragment.warnings.empty()) {
    std::filesystem::remove_all(root);
    return 1;
  }

  // Pathologically deep nesting (far past any real code) would recurse one native
  // stack frame per level in walk_node and blow the ~512 KB extraction-worker
  // stack, killing the process with SIGBUS. The depth cap must engage: the file
  // still yields its shallow nodes (the function), records the ast-depth-cap
  // warning, and never crashes. The cap is thread-independent, so the result is
  // identical whether this runs on the 8 MB main thread or a 512 KB worker.
  std::string deep_source = "function deep() { return ";
  for (int index = 0; index < 4000; ++index) {
    deep_source += "(";
  }
  deep_source += "0";
  for (int index = 0; index < 4000; ++index) {
    deep_source += ")";
  }
  deep_source += "; }\n";
  const auto deep_path = root / "deep.js";
  write_file(deep_path, deep_source);
  const auto deep_result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = deep_path,
      .language = cgraph::DetectedLanguage::JavaScript,
  }, root);
  if (deep_result.fragment.nodes.empty() || !contains_warning(deep_result, "ast-depth-cap")) {
    std::filesystem::remove_all(root);
    return 1;
  }

  // Names that are not valid UTF-8 must still get distinct, non-empty node ids.
  // They used to normalize to the empty id, and the second colliding member sent
  // unique_node_id into an endless loop: this 44-byte file hung indexing for good.
  const auto bad_field_path = root / "bad_field.proto";
  write_file(bad_field_path, "message M {\n string \xDA = 1;\n string \xDA = 1;\n}\n");
  const auto bad_field_result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = bad_field_path,
      .language = cgraph::DetectedLanguage::Protobuf,
  }, root);
  if (bad_field_result.fragment.nodes.empty() || !distinct_non_empty_ids(bad_field_result)) {
    std::filesystem::remove_all(root);
    return 1;
  }

#ifdef __linux__
  // The same for a file whose own name is not UTF-8. Linux file names are raw
  // bytes; macOS and Windows refuse to create this one.
  const auto bad_name_path = root / "\xFF\xFE.py";
  write_file(bad_name_path, "def first():\n    return 1\n\ndef second():\n    return 2\n");
  const auto bad_name_result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = bad_name_path,
      .language = cgraph::DetectedLanguage::Python,
  }, root);
  if (bad_name_result.fragment.nodes.size() < 3 || !distinct_non_empty_ids(bad_name_result)) {
    std::filesystem::remove_all(root);
    return 1;
  }
#endif

  constexpr std::size_t oversized_bytes = (8 * 1024 * 1024) + 1;
  const auto oversized_path = root / "oversized.py";
  write_file(oversized_path, std::string(oversized_bytes, 'x'));
  const auto oversized_result = cgraph::extract_detected_file(cgraph::DetectedFile{
      .path = oversized_path,
      .language = cgraph::DetectedLanguage::Python,
  }, root);
  if (!oversized_result.fragment.nodes.empty() || !contains_warning(oversized_result, "file too large")) {
    std::filesystem::remove_all(root);
    return 1;
  }

  std::filesystem::remove_all(root);
  return 0;
}
