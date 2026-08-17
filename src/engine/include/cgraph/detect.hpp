#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

enum class DetectedLanguage {
  Unknown,
  Apex,
  C,
  Cpp,
  CSharp,
  Delphi,
  Go,
  Groovy,
  Java,
  JavaScript,
  Kotlin,
  McpConfig,
  MsBuild,
  PhpBlade,
  Python,
  Ruby,
  Rust,
  Scala,
  Sql,
  TypeScript,
  Tsx,
  Xml,
};

struct DetectedFile {
  std::filesystem::path path;
  DetectedLanguage language = DetectedLanguage::Unknown;
};

[[nodiscard]] DetectedLanguage detect_language(const std::filesystem::path& path);
// Stable lowercase display name for a detected language ("go", "csharp",
// "php-blade", ...). Used as the key in the `unextracted` coverage maps.
[[nodiscard]] std::string_view language_name(DetectedLanguage language);
[[nodiscard]] std::vector<DetectedFile> detect_project_files(const std::filesystem::path& root);

}  // namespace cgraph
