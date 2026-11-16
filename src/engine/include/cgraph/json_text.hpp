#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <istream>
#include <limits>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

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

// How deeply arrays and objects may nest in JSON this program reads. Real
// inputs (requests, manifests, OpenAPI documents, saved graphs) stay far below
// it; the limit is what keeps every recursive walk, copy and dump of a parsed
// value within a small, fixed amount of stack.
inline constexpr std::size_t kMaxJsonDepth = 200;

enum class JsonComments { Rejected, Ignored };

namespace json_detail {

// True when arrays and objects in `text` nest deeper than `limit`. One
// pass over the bytes, before any value is built: nlohmann's parser does not
// recurse, so it will happily build a value 200,000 levels deep from a few
// hundred kilobytes, and copying, comparing or dumping that value then
// recurses once per level and overflows the stack. Brackets inside strings
// (and inside comments, when those are allowed) do not count.
[[nodiscard]] inline bool nests_too_deep(std::string_view text, JsonComments comments, std::size_t limit) {
  std::size_t depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    switch (text[i]) {
      case '"':
        for (++i; i < text.size() && text[i] != '"'; ++i) {
          if (text[i] == '\\') ++i;  // the escaped character is never a closing quote
        }
        break;
      case '/':
        // Comments end exactly where the library's lexer ends them: a line
        // comment at \n, \r or NUL, a block comment after its closing `*/`.
        // Ending one anywhere else would let brackets hide behind it.
        if (comments == JsonComments::Ignored && i + 1 < text.size()) {
          if (text[i + 1] == '/') {
            i = text.find_first_of(std::string_view("\n\r\0", 3), i);
          } else if (text[i + 1] == '*') {
            i = text.find("*/", i + 2);
            if (i != std::string_view::npos) ++i;  // onto the closing `/`
          }
          if (i == std::string_view::npos) return false;  // comment runs to the end
        }
        break;
      case '[':
      case '{':
        if (++depth > limit) return true;
        break;
      case ']':
      case '}':
        if (depth > 0) --depth;
        break;
      default:
        break;
    }
  }
  return false;
}

[[nodiscard]] inline nlohmann::json parse(std::string_view text, bool allow_exceptions, JsonComments comments,
                                          std::size_t limit = kMaxJsonDepth) {
  if (nests_too_deep(text, comments, limit)) {
    if (allow_exceptions) {
      throw nlohmann::json::parse_error::create(
          101, std::size_t{0}, "JSON nests deeper than " + std::to_string(limit) + " levels", nullptr);
    }
    return nlohmann::json(nlohmann::json::value_t::discarded);
  }
  return nlohmann::json::parse(text, nullptr, allow_exceptions, comments == JsonComments::Ignored);
}

[[nodiscard]] inline std::string slurp(std::istream& input) {
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace json_detail

// The one way to turn text into JSON. Every input is checked against the
// nesting limit above before it is parsed. `parse_json` throws
// nlohmann::json::parse_error on malformed or too deeply nested input;
// `try_parse_json` returns a discarded value (`is_discarded()`) instead. Both
// take text or a stream, which is read to its end. `read_json` parses the
// first value in a stream and ignores anything after it, as `stream >> json`
// did.
[[nodiscard]] inline nlohmann::json parse_json(std::string_view text, JsonComments comments = JsonComments::Rejected) {
  return json_detail::parse(text, /*allow_exceptions=*/true, comments);
}
[[nodiscard]] inline nlohmann::json parse_json(std::istream& input, JsonComments comments = JsonComments::Rejected) {
  return json_detail::parse(json_detail::slurp(input), /*allow_exceptions=*/true, comments);
}

[[nodiscard]] inline nlohmann::json try_parse_json(std::string_view text, JsonComments comments = JsonComments::Rejected) {
  return json_detail::parse(text, /*allow_exceptions=*/false, comments);
}
[[nodiscard]] inline nlohmann::json try_parse_json(std::istream& input, JsonComments comments = JsonComments::Rejected) {
  return json_detail::parse(json_detail::slurp(input), /*allow_exceptions=*/false, comments);
}

// A request frame: `params` may use the whole limit, and the frame is the one
// object around it. Without the extra level, params the client accepted at
// exactly the limit made a frame the daemon dropped unanswered, which the
// client reads as "no daemon".
[[nodiscard]] inline nlohmann::json parse_json_frame(std::string_view text) {
  return json_detail::parse(text, /*allow_exceptions=*/true, JsonComments::Rejected, kMaxJsonDepth + 1);
}

[[nodiscard]] inline nlohmann::json read_json(std::istream& input) {
  const auto text = json_detail::slurp(input);
  if (json_detail::nests_too_deep(text, JsonComments::Rejected, kMaxJsonDepth)) {
    throw nlohmann::json::parse_error::create(
        101, std::size_t{0}, "JSON nests deeper than " + std::to_string(kMaxJsonDepth) + " levels", nullptr);
  }
  std::istringstream first_value(text);
  nlohmann::json value;
  first_value >> value;
  return value;
}

// A whole-number request parameter (`limit`, `budget`, `max_depth`, ...), or
// `fallback` when the key is absent.
//
// A caller may send any JSON number, so `{"max_depth": 1e300}` has to mean
// something. `params.value(key, fallback)` converts a float with a bare cast,
// which is undefined when the value does not fit the integer type. Here a
// float is cut to its whole part as before, clamped to the type's range first;
// NaN reads as absent. Integers convert as they always did, and a value that
// is not a number still throws nlohmann::json::type_error for the caller to
// report.
template <typename Integer>
[[nodiscard]] Integer integer_param(const nlohmann::json& params, const char* key, Integer fallback) {
  static_assert(std::is_integral_v<Integer>);
  const auto found = params.find(key);
  if (found == params.end()) {
    return fallback;
  }
  if (!found->is_number_float()) {
    return found->template get<Integer>();
  }
  const auto value = found->template get<double>();
  if (std::isnan(value)) {
    return fallback;
  }
  // The limits as doubles round outward (2^63, 2^64), so comparing with >= and
  // <= keeps every value that reaches the cast inside the type's range.
  if (value <= static_cast<double>(std::numeric_limits<Integer>::min())) {
    return std::numeric_limits<Integer>::min();
  }
  if (value >= static_cast<double>(std::numeric_limits<Integer>::max())) {
    return std::numeric_limits<Integer>::max();
  }
  return static_cast<Integer>(value);
}

}  // namespace cgraph
