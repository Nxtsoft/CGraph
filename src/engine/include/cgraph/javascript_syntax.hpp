#pragma once

// Syntax helpers the JavaScript/TypeScript extractor and its HTTP consumer
// reading (http_consumers.cpp) share. Defined in javascript_extractor.cpp.

#include "cgraph/language_config.hpp"

#include <tree_sitter/api.h>

#include <string>
#include <string_view>
#include <vector>

namespace cgraph::js_syntax {

[[nodiscard]] std::string node_text(const TSNode& node, std::string_view source);
[[nodiscard]] std::string field_text(const TSNode& node, const char* field, std::string_view source);
[[nodiscard]] bool is_function_value(const TSNode& node);
[[nodiscard]] bool is_string_value(const TSNode& node);
[[nodiscard]] bool is_type_wrapper(std::string_view type);
[[nodiscard]] TSNode unwrap_expression(TSNode node);
[[nodiscard]] bool is_function_node(std::string_view type);
void parameter_names(const TSNode& function, std::string_view source, std::vector<std::string>& out);
[[nodiscard]] std::string strip_string_quotes(std::string value);
[[nodiscard]] bool is_module_level_declaration(const TSNode& node);

// The code an expression at `node` belongs to, as a fact's source: the
// enclosing function (`function_scope_id`) when there is one, else the
// module-level variable whose initializer holds it when the extractor made a
// node for it (`export const notebooksApi = { list: () => apiFetch('/x') }`:
// the arrow is a boundary, the object is the symbol), else the file. A
// module-level variable with no node (`const API = process.env.X || '…'`)
// would leave the fact with no node to hang on, so the file reads instead.
[[nodiscard]] std::string reading_scope_id(const TSNode& node, const ExtractionContext& context,
                                           const std::string& function_scope_id, const Fragment& fragment);

}  // namespace cgraph::js_syntax
