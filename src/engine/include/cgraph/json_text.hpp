#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace cgraph {

// The one way to turn JSON into text. Labels, snippets and paths come from
// source files and file names, which are raw bytes and not always UTF-8, while
// JSON text must be. nlohmann's default `dump()` throws on such a string, which
// took down a whole index build or a daemon reply over one odd byte; here each
// invalid byte becomes U+FFFD and everything else is written unchanged.
//
// The replacement loses the original bytes, so a path or label that is not
// UTF-8 does not read back from disk as it was written. Two known costs:
// the saved index never matches a project holding a non-UTF-8 file name, so
// such a project is re-extracted in full on each daemon start; and two strings
// that differ only in invalid bytes read back equal.
[[nodiscard]] inline std::string dump_json(const nlohmann::json& value, int indent = -1) {
  return value.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace cgraph
