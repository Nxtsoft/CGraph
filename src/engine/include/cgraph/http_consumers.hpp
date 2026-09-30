#pragma once

// HTTP client calls in JavaScript/TypeScript (contracts.hpp: http_call /
// http_wrapper / http_call_args / url_const facts), run by the JavaScript
// extractor's walk over every node.

#include "cgraph/extractor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cgraph {

// `fetch(url, opts)`, `api.GET('/path')`, `axios.post(url)`, calls to wrappers,
// and functions that are wrappers themselves.
void http_call_handler(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                       std::vector<RawRelation>& out);

// Held while one file is extracted: remembers which of its functions are
// wrappers so a file with many `this.x()` calls reads each function once.
// Scopes nest; each covers one file on its thread.
class HttpConsumerFileScope {
 public:
  HttpConsumerFileScope();
  ~HttpConsumerFileScope();
  HttpConsumerFileScope(const HttpConsumerFileScope&) = delete;
  HttpConsumerFileScope& operator=(const HttpConsumerFileScope&) = delete;

 private:
  struct Cache;
  std::unique_ptr<Cache> cache_;
};

// Module-level string constants that read as a URL or a URL prefix.
void url_const_handler(const TSNode& node, const ExtractionContext& context, std::vector<RawRelation>& out);

}  // namespace cgraph
