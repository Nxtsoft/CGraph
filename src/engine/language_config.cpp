#include "cgraph/language_config.hpp"

#include "cgraph/normalize.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace cgraph {
namespace {

[[nodiscard]] std::vector<TSSymbol> intern_many(const TSLanguage* language, const std::vector<std::string>& names) {
  std::vector<TSSymbol> symbols;
  symbols.reserve(names.size());

  for (const auto& name : names) {
    auto symbol = ts_language_symbol_for_name(
        language,
        name.data(),
        static_cast<std::uint32_t>(name.size()),
        true);
    if (symbol == 0) {
      symbol = ts_language_symbol_for_name(
          language,
          name.data(),
          static_cast<std::uint32_t>(name.size()),
          false);
    }
    if (symbol != 0 && !contains_symbol(symbols, symbol)) {
      symbols.push_back(symbol);
    }
  }

  std::ranges::sort(symbols);
  return symbols;
}

// Positions in fragment.nodes by id, ascending, for the first `synced` nodes.
struct IdIndex {
  const Fragment* fragment = nullptr;
  std::size_t synced = 0;
  std::unordered_map<std::string, std::vector<std::size_t>> positions;
};

thread_local IdIndex* current_id_index = nullptr;

// The held index over `fragment`, caught up with the nodes appended since the
// last lookup, or nullptr when no scope covers `fragment`.
[[nodiscard]] IdIndex* id_index_for(const Fragment& fragment) {
  auto* index = current_id_index;
  if (index == nullptr || index->fragment != &fragment) {
    return nullptr;
  }
  for (; index->synced < fragment.nodes.size(); ++index->synced) {
    index->positions[fragment.nodes[index->synced].id].push_back(index->synced);
  }
  return index;
}

}  // namespace

struct NodeIdIndexScope::Index {
  IdIndex ids;
  IdIndex* previous = nullptr;
};

NodeIdIndexScope::NodeIdIndexScope(const Fragment& fragment) : index_(std::make_unique<Index>()) {
  index_->ids.fragment = &fragment;
  index_->previous = current_id_index;
  current_id_index = &index_->ids;
}

NodeIdIndexScope::~NodeIdIndexScope() { current_id_index = index_->previous; }

bool node_id_taken(const Fragment& fragment, const std::string& id) {
  if (const auto* index = id_index_for(fragment)) {
    return index->positions.contains(id);
  }
  return std::ranges::any_of(fragment.nodes, [&id](const Node& existing) { return existing.id == id; });
}

std::size_t find_node_by_id(const Fragment& fragment, const std::string& id, std::string_view kind) {
  if (const auto* index = id_index_for(fragment)) {
    const auto found = index->positions.find(id);
    if (found == index->positions.end()) {
      return fragment.nodes.size();
    }
    const auto position = std::ranges::find_if(
        found->second, [&](std::size_t candidate) { return fragment.nodes[candidate].kind == kind; });
    return position == found->second.end() ? fragment.nodes.size() : *position;
  }
  const auto position = std::ranges::find_if(
      fragment.nodes, [&](const Node& existing) { return existing.id == id && existing.kind == kind; });
  return static_cast<std::size_t>(position - fragment.nodes.begin());
}

void set_node_id(Fragment& fragment, std::size_t position, std::string id) {
  if (auto* index = id_index_for(fragment)) {
    const auto old = index->positions.find(fragment.nodes[position].id);
    std::erase(old->second, position);
    if (old->second.empty()) {
      index->positions.erase(old);
    }
    auto& positions = index->positions[id];
    positions.insert(std::ranges::upper_bound(positions, position), position);
  }
  fragment.nodes[position].id = std::move(id);
}

void intern_node_symbols(LanguageConfig& config, const TSLanguage* language) {
  config.symbols.class_nodes = intern_many(language, config.class_node_types);
  config.symbols.function_nodes = intern_many(language, config.function_node_types);
  config.symbols.method_nodes = intern_many(language, config.method_node_types);
  config.symbols.type_nodes = intern_many(language, config.type_node_types);
  config.symbols.import_nodes = intern_many(language, config.import_node_types);
  config.symbols.call_nodes = intern_many(language, config.call_node_types);
}

bool contains_symbol(const std::vector<TSSymbol>& symbols, TSSymbol symbol) {
  return std::ranges::find(symbols, symbol) != symbols.end();
}

std::string unique_node_id(
    const std::string& seed, const SourceLocation& location, const Fragment& fragment) {
  const auto taken = [&fragment](const std::string& candidate) { return node_id_taken(fragment, candidate); };
  auto id = make_id(seed);
  if (!taken(id)) {
    return id;
  }
  // Three overloads can share one line (`int f(int); int f(double);`), so the
  // line alone can still collide; add the column, then a counter.
  const auto base = seed + ":" + std::to_string(location.start_line);
  id = make_id(base + ":" + std::to_string(location.start_column));
  for (std::size_t nth = 2; taken(id); ++nth) {
    id = make_id(base + ":" + std::to_string(location.start_column) + ":" + std::to_string(nth));
  }
  return id;
}

std::string add_field_node(
    const ExtractionContext& context,
    const std::string& owner_id,
    std::string_view owner_name,
    std::string label,
    const SourceLocation& location,
    Properties properties,
    Fragment& fragment) {
  if (label.empty() || owner_name.empty() || owner_id.empty()) {
    return {};
  }
  auto id = unique_node_id(
      context.relative_path + ":" + std::string(owner_name) + "::" + label, location, fragment);
  fragment.nodes.push_back(Node{
      .id = id,
      .label = std::move(label),
      .source_file = context.source_file,
      .source_location = location,
      .kind = "field",
      .confidence = Confidence::Extracted,
      .properties = std::move(properties),
  });
  fragment.edges.push_back(Edge{
      .source = owner_id,
      .target = id,
      .relation = "defines",
      .confidence = Confidence::Extracted,
  });
  return id;
}

}  // namespace cgraph
