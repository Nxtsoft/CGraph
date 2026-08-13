#include "cgraph/semantic_fragment_validation.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/fragment_json.hpp"

#include <fstream>
#include <iterator>
#include <utility>

namespace cgraph {

SemanticFragmentValidationResult validate_semantic_fragment_json(const nlohmann::json& value) {
  SemanticFragmentValidationResult result;
  result.valid = parse_fragment(value, result.fragment, result.errors);
  return result;
}

SemanticFragmentValidationResult validate_semantic_fragment_file(const std::filesystem::path& path) {
  SemanticFragmentValidationResult result;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.errors.push_back("failed to open semantic fragment: " + path.generic_string());
    return result;
  }

  const std::string contents{
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    result.errors.push_back("failed to read semantic fragment: " + path.generic_string());
    return result;
  }
  result.source_sha256 = sha256_hex(contents);

  const auto value = nlohmann::json::parse(contents, nullptr, false);
  if (value.is_discarded()) {
    result.errors.push_back("semantic fragment is malformed JSON: " + path.generic_string());
    return result;
  }

  auto validated = validate_semantic_fragment_json(value);
  validated.source_sha256 = std::move(result.source_sha256);
  return validated;
}

}  // namespace cgraph
