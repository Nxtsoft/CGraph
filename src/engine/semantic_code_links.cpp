#include "cgraph/semantic_code_links.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>

namespace cgraph {
namespace {

constexpr std::size_t kMinNameLength = 2;

// How many nodes one mentioned name may contribute to a document's candidate
// links. Bounds the fan-out of a name shared by many nodes without discarding
// the mention altogether; see the emission loop for why.
constexpr std::size_t kMaxLinksPerName = 3;

// Node kinds whose capitalized single-word names are deliberate type references
// (so `Fragment`, `GraphSnapshot`, `Node` survive the shape filter even without
// an internal case change).
[[nodiscard]] bool is_type_like_kind(std::string_view kind) {
  static constexpr std::array<std::string_view, 9> kTypeKinds = {
      "class", "struct", "interface", "enum", "enum_class", "type", "typedef", "trait", "record"};
  return std::ranges::find(kTypeKinds, kind) != kTypeKinds.end();
}

[[nodiscard]] bool is_ident_start(unsigned char c) { return std::isalpha(c) != 0 || c == '_'; }
[[nodiscard]] bool is_ident_char(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

// The leading identifier of a label: "handle_request(Foo&)" -> "handle_request",
// "GraphSnapshot" -> "GraphSnapshot". Empty if the label doesn't start with one.
[[nodiscard]] std::string_view leading_identifier(std::string_view label) {
  if (label.empty() || !is_ident_start(static_cast<unsigned char>(label.front()))) {
    return {};
  }
  std::size_t end = 1;
  while (end < label.size() && is_ident_char(static_cast<unsigned char>(label[end]))) {
    ++end;
  }
  return label.substr(0, end);
}

// snake_case (has '_') or internal CamelCase (a lowercase immediately followed by
// an uppercase) — almost never a coincidental English word.
[[nodiscard]] bool is_compound_identifier(std::string_view name) {
  if (name.find('_') != std::string_view::npos) {
    return true;
  }
  for (std::size_t i = 1; i < name.size(); ++i) {
    if (std::islower(static_cast<unsigned char>(name[i - 1])) != 0 &&
        std::isupper(static_cast<unsigned char>(name[i])) != 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool is_capitalized(std::string_view name) {
  return !name.empty() && std::isupper(static_cast<unsigned char>(name.front())) != 0;
}

}  // namespace

SymbolIndex build_symbol_index(const GraphSnapshot& graph) {
  SymbolIndex index;
  for (const auto& node : graph.nodes) {
    const auto name = leading_identifier(node.label);
    if (name.size() < kMinNameLength) {
      continue;
    }
    auto& entry = index.by_name[std::string(name)];
    entry.nodes.push_back(CandidateLink{.node_id = node.id, .label = node.label});
    if (is_type_like_kind(node.kind)) {
      entry.type_like = true;
    }
  }
  return index;
}

std::vector<CandidateLink> compute_candidate_links(
    std::string_view doc_text,
    const SymbolIndex& index,
    std::size_t max_links) {
  // Distinct identifier tokens in the document.
  std::unordered_set<std::string> seen;
  struct Ranked {
    std::string name;
    const SymbolEntry* entry;
  };
  std::vector<Ranked> ranked;

  for (std::size_t i = 0; i < doc_text.size();) {
    if (!is_ident_start(static_cast<unsigned char>(doc_text[i]))) {
      ++i;
      continue;
    }
    std::size_t end = i + 1;
    while (end < doc_text.size() && is_ident_char(static_cast<unsigned char>(doc_text[end]))) {
      ++end;
    }
    std::string token(doc_text.substr(i, end - i));
    i = end;
    if (token.size() < kMinNameLength || !seen.insert(token).second) {
      continue;
    }
    const auto found = index.by_name.find(token);
    if (found == index.by_name.end() || found->second.nodes.empty()) {
      continue;
    }
    // Shape filter: keep compound identifiers and capitalized type names; drop
    // bare lowercase words that merely collide with a symbol name.
    const bool keep = is_compound_identifier(token) || (is_capitalized(token) && found->second.type_like);
    if (keep) {
      ranked.push_back(Ranked{.name = std::move(token), .entry = &found->second});
    }
  }

  // Rarer names first (stronger signal), then longer token (more specific),
  // then name for a stable, deterministic order.
  std::ranges::sort(ranked, [](const Ranked& lhs, const Ranked& rhs) {
    if (lhs.entry->nodes.size() != rhs.entry->nodes.size()) {
      return lhs.entry->nodes.size() < rhs.entry->nodes.size();
    }
    if (lhs.name.size() != rhs.name.size()) {
      return lhs.name.size() > rhs.name.size();
    }
    return lhs.name < rhs.name;
  });

  std::vector<CandidateLink> links;
  for (const auto& item : ranked) {
    // Cap what one name contributes. A mention of a name carried by N nodes is
    // evidence for at most one of them, so emitting all N spends the document's
    // budget on N-1 links that are wrong by construction -- `write_file` names
    // 37 nodes in this repo's own graph. Emitting a few has the same chance of
    // including the right node as emitting all of them, and leaves room for the
    // rarer (more specific) names ranked behind it.
    std::size_t from_this_name = 0;
    for (const auto& node : item.entry->nodes) {
      if (links.size() >= max_links) {
        return links;
      }
      if (from_this_name >= kMaxLinksPerName) {
        break;
      }
      links.push_back(node);
      ++from_this_name;
    }
  }
  return links;
}

}  // namespace cgraph
