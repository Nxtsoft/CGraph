#include "cgraph/language_config.hpp"

#include <algorithm>
#include <cstdint>

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

}  // namespace

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

}  // namespace cgraph
