#pragma once

// Syntax helpers the JavaScript/TypeScript extractor and its HTTP consumer
// reading (http_consumers.cpp) share. Defined in javascript_extractor.cpp.

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

}  // namespace cgraph::js_syntax
