#pragma once

// HTTP client calls in JavaScript/TypeScript (contracts.hpp: http_call /
// http_wrapper / http_call_args / url_const facts), run by the JavaScript
// extractor's walk over every node.

#include "cgraph/extractor.hpp"

#include <string>
#include <vector>

namespace cgraph {

// `fetch(url, opts)`, `api.GET('/path')`, `axios.post(url)`, calls to wrappers,
// and functions that are wrappers themselves.
void http_call_handler(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                       std::vector<RawRelation>& out);

// Module-level string constants that read as a URL or a URL prefix.
void url_const_handler(const TSNode& node, const ExtractionContext& context, std::vector<RawRelation>& out);

}  // namespace cgraph
