#include "cgraph/detect.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, std::string contents = {}) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

bool contains_language(const std::vector<cgraph::DetectedFile>& files, cgraph::DetectedLanguage language) {
  for (const auto& file : files) {
    if (file.language == language) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  using cgraph::DetectedLanguage;

  if (cgraph::detect_language("component.blade.php") != DetectedLanguage::PhpBlade) {
    return 1;
  }
  if (cgraph::detect_language("mcp.json") != DetectedLanguage::McpConfig) {
    return 1;
  }
  if (cgraph::detect_language("project.csproj") != DetectedLanguage::MsBuild) {
    return 1;
  }
  if (cgraph::detect_language("main.cpp") != DetectedLanguage::Cpp) {
    return 1;
  }
  if (cgraph::detect_language("tool.py") != DetectedLanguage::Python) {
    return 1;
  }
  // .sql is a recognized language (file-level), so it is detected as code rather
  // than left Unknown (which would make it an enrichment-only document).
  // Contract documents: OpenAPI by name (JSON only), proto and GraphQL by extension.
  if (cgraph::detect_language("openapi.json") != DetectedLanguage::OpenApi ||
      cgraph::detect_language("docs/Swagger.v2.JSON") != DetectedLanguage::OpenApi ||
      cgraph::detect_language("petstore.openapi.json") != DetectedLanguage::OpenApi ||
      cgraph::detect_language("openapi.yaml") != DetectedLanguage::Unknown ||
      cgraph::detect_language("package.json") != DetectedLanguage::Unknown ||
      cgraph::detect_language("api/notes.proto") != DetectedLanguage::Protobuf ||
      cgraph::detect_language("schema.graphql") != DetectedLanguage::GraphQL ||
      cgraph::detect_language("schema.gql") != DetectedLanguage::GraphQL ||
      cgraph::detect_language("schema.graphqls") != DetectedLanguage::GraphQL) {
    return 1;
  }
  if (cgraph::detect_language("migration.sql") != DetectedLanguage::Sql) {
    return 1;
  }

  const auto root = std::filesystem::temp_directory_path() / "cgraph_detect_test";
  std::filesystem::remove_all(root);
  write_file(root / ".gitignore", "ignored.py\nbuild/\n");
  write_file(root / "src" / "main.cpp");
  write_file(root / "src" / "tool.py");
  write_file(root / "ignored.py");
  write_file(root / "build" / "generated.cpp");
  write_file(root / "README.md");

  const auto files = cgraph::detect_project_files(root);
  std::filesystem::remove_all(root);

  if (!contains_language(files, DetectedLanguage::Cpp)) {
    return 1;
  }
  if (!contains_language(files, DetectedLanguage::Python)) {
    return 1;
  }
  if (files.size() != 2) {
    return 1;
  }

  return 0;
}
