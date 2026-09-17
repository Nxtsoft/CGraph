#include "cgraph/fingerprint.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace cgraph {
namespace {

// Grammar node types are language-specific, but the ones that carry a name or
// a literal share a vocabulary across every tree-sitter grammar vendored here.
[[nodiscard]] bool is_identifier_type(std::string_view type) {
  return type == "identifier" || type.ends_with("_identifier") || type == "name" || type == "simple_identifier" ||
         type == "constant" || type == "shorthand_property_identifier_pattern";
}

[[nodiscard]] bool is_literal_type(std::string_view type) {
  // Boolean and null keywords are literals too: `{recursive: true}` and
  // `{recursive: false}` are the same shape with a different constant.
  if (type == "true" || type == "false" || type == "null" || type == "undefined" || type == "none" || type == "nil" ||
      type == "True" || type == "False" || type == "None" || type == "nullptr" || type == "null_literal" ||
      type == "boolean_literal") {
    return true;
  }
  return type.find("string") != std::string_view::npos || type.find("number") != std::string_view::npos ||
         type.find("integer") != std::string_view::npos || type.find("float") != std::string_view::npos ||
         type.find("char") != std::string_view::npos || type.ends_with("_literal") || type == "literal" ||
         type == "template_string" || type == "regex" || type == "raw_string_literal";
}

[[nodiscard]] bool is_comment_type(std::string_view type) {
  return type.find("comment") != std::string_view::npos;
}

void collect_tokens(TSNode node, std::string_view source, std::vector<std::string>& out) {
  const std::string_view type = ts_node_type(node);
  if (is_comment_type(type)) {
    return;
  }
  if (ts_node_is_named(node)) {
    if (is_literal_type(type)) {
      out.emplace_back("LIT");
      return;
    }
    if (is_identifier_type(type)) {
      out.emplace_back("ID");
      return;
    }
  }
  const auto child_count = ts_node_child_count(node);
  if (child_count == 0) {
    const auto start = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    if (end > start && end <= source.size()) {
      out.emplace_back(source.substr(start, end - start));
    }
    return;
  }
  for (std::uint32_t index = 0; index < child_count; ++index) {
    collect_tokens(ts_node_child(node, index), source, out);
  }
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view text, std::uint64_t hash) {
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

}  // namespace

std::vector<std::string> normalized_tokens(TSNode node, std::string_view source) {
  std::vector<std::string> tokens;
  if (!ts_node_is_null(node)) {
    collect_tokens(node, source, tokens);
  }
  return tokens;
}

FunctionFingerprint fingerprint_function(TSNode node, std::string_view source) {
  FunctionFingerprint fingerprint;
  const auto tokens = normalized_tokens(node, source);
  fingerprint.tokens = static_cast<std::uint32_t>(tokens.size());
  if (tokens.empty()) {
    return fingerprint;
  }
  // Shingle hashes over every run of kShingleSize tokens; a body shorter than
  // one shingle is a single shingle of everything it has.
  std::vector<std::uint64_t> shingles;
  const auto count = tokens.size() >= kShingleSize ? tokens.size() - kShingleSize + 1 : 1;
  shingles.reserve(count);
  for (std::size_t start = 0; start < count; ++start) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t offset = 0; offset < kShingleSize && start + offset < tokens.size(); ++offset) {
      hash = fnv1a(tokens[start + offset], hash);
      hash = fnv1a(std::string_view("\x1f", 1), hash);
    }
    shingles.push_back(hash);
  }
  // Winnowing (Schleimer, Wilkerson, Aiken): the minimum of each window,
  // rightmost on a tie, recorded once per distinct position.
  std::vector<std::uint64_t> selected;
  const auto window = std::min(kWinnowWindow, shingles.size());
  std::size_t last_selected = static_cast<std::size_t>(-1);
  for (std::size_t start = 0; start + window <= shingles.size(); ++start) {
    std::size_t best = start;
    for (std::size_t i = start; i < start + window; ++i) {
      if (shingles[i] <= shingles[best]) {
        best = i;
      }
    }
    if (best != last_selected) {
      selected.push_back(shingles[best]);
      last_selected = best;
    }
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  fingerprint.shingles = std::move(selected);
  return fingerprint;
}

double fingerprint_similarity(const FunctionFingerprint& a, const FunctionFingerprint& b) {
  std::size_t shared = 0;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.shingles.size() && j < b.shingles.size()) {
    if (a.shingles[i] == b.shingles[j]) {
      ++shared;
      ++i;
      ++j;
    } else if (a.shingles[i] < b.shingles[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  const auto union_size = a.shingles.size() + b.shingles.size() - shared;
  return union_size == 0 ? 0.0 : static_cast<double>(shared) / static_cast<double>(union_size);
}

}  // namespace cgraph
