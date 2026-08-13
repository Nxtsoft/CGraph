#pragma once

#include "cgraph/types.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace cgraph {

struct SemanticFragmentValidationResult {
  bool valid = false;
  Fragment fragment;
  // SHA-256 of the exact byte buffer parsed by validate_semantic_fragment_file.
  // Empty for in-memory JSON validation or when the file could not be read.
  std::string source_sha256;
  std::vector<std::string> errors;
};

[[nodiscard]] SemanticFragmentValidationResult validate_semantic_fragment_json(const nlohmann::json& value);
[[nodiscard]] SemanticFragmentValidationResult validate_semantic_fragment_file(const std::filesystem::path& path);

}  // namespace cgraph
