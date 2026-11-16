#include "cgraph/json_text.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace {

bool ok = true;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

// `depth` arrays inside one another around a single 1.
std::string nested_arrays(std::size_t depth) {
  return std::string(depth, '[') + "1" + std::string(depth, ']');
}

std::string nested_objects(std::size_t depth) {
  std::string text;
  for (std::size_t i = 0; i < depth; ++i) text += "{\"a\":";
  return text + "1" + std::string(depth, '}');
}

bool throws_parse_error(const std::string& text) {
  try {
    (void)cgraph::parse_json(text);
  } catch (const nlohmann::json::parse_error&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  using cgraph::kMaxJsonDepth;

  // Ordinary documents come out exactly as the library's own parser gives them.
  const std::string document =
      R"({"name":"x","list":[1,2.5,-3,true,null,"s"],"nested":{"k":{"deep":[[],{}]}},"text":"caf\u00e9"})";
  expect(cgraph::parse_json(document) == nlohmann::json::parse(document), "parse_json matches the library parser");
  expect(cgraph::try_parse_json(document) == nlohmann::json::parse(document), "try_parse_json matches the library parser");
  expect(cgraph::parse_json("7") == 7 && cgraph::parse_json("\"s\"") == "s" && cgraph::parse_json("[]").empty(),
         "scalars and empty containers");

  // The limit counts containers, of either kind, and applies at the boundary.
  expect(cgraph::parse_json(nested_arrays(kMaxJsonDepth)).is_array(), "arrays at the limit parse");
  expect(cgraph::parse_json(nested_objects(kMaxJsonDepth)).is_object(), "objects at the limit parse");
  expect(throws_parse_error(nested_arrays(kMaxJsonDepth + 1)), "arrays one past the limit are refused");
  expect(throws_parse_error(nested_objects(kMaxJsonDepth + 1)), "objects one past the limit are refused");
  expect(cgraph::try_parse_json(nested_arrays(kMaxJsonDepth + 1)).is_discarded(), "try_parse_json discards past the limit");
  // Depth is how far inside, not how many: a long flat list of small arrays is fine.
  std::string wide = "[";
  for (int i = 0; i < 5000; ++i) wide += (i ? ",[[1]]" : "[[1]]");
  expect(cgraph::parse_json(wide + "]").size() == 5000, "many shallow siblings do not count as depth");

  // The inputs that used to take the process down: a few hundred kilobytes
  // nesting 200,000 deep. The parser built them without complaint, and the
  // first copy or dump of the result overflowed the stack.
  const auto deep = nested_arrays(200000);
  expect(throws_parse_error(deep), "200,000 levels of arrays are refused");
  expect(cgraph::try_parse_json(deep).is_discarded(), "200,000 levels are discarded, not built");
  expect(cgraph::try_parse_json("{\"params\":{\"a\":" + deep + "}}").is_discarded(), "deep value inside a request");
  // What does parse can always be copied and written back out.
  const auto at_limit = cgraph::parse_json(nested_objects(kMaxJsonDepth));
  const auto copy = at_limit;
  expect(cgraph::parse_json(cgraph::dump_json(copy)) == at_limit, "a value at the limit copies and round-trips");

  // Malformed input: an exception from one, a discarded value from the other.
  expect(throws_parse_error("{not json") && throws_parse_error("") && throws_parse_error("[1,2] trailing"),
         "malformed input throws");
  expect(cgraph::try_parse_json("{not json").is_discarded() && cgraph::try_parse_json("").is_discarded(),
         "malformed input is discarded");

  // Only real nesting counts: brackets inside a string are text, and an
  // escaped quote does not end the string they are in.
  const std::string brackets(1000, '[');
  expect(cgraph::parse_json("[\"" + brackets + "\"]")[0] == brackets, "brackets inside a string are not depth");
  expect(cgraph::parse_json("[\"a\\\"" + brackets + "\"]").is_array(), "an escaped quote keeps the string open");
  expect(cgraph::parse_json("[\"a\\\\\"," + nested_arrays(kMaxJsonDepth - 1) + "]").size() == 2,
         "an escaped backslash before the closing quote ends the string");
  expect(cgraph::try_parse_json("// " + brackets + "\n[1]", cgraph::JsonComments::Ignored) == nlohmann::json{1} &&
             cgraph::try_parse_json("/* " + brackets + " */[1]", cgraph::JsonComments::Ignored) == nlohmann::json{1},
         "brackets inside a comment are not depth when comments are allowed");

  // Comments are an error unless asked for (tsconfig.json has them).
  const std::string commented = "{\n  // note\n  \"a\": 1 /* inline */\n}";
  expect(cgraph::try_parse_json(commented).is_discarded(), "comments rejected by default");
  expect(cgraph::try_parse_json(commented, cgraph::JsonComments::Ignored) == nlohmann::json{{"a", 1}}, "comments ignored on request");

  // Streams are read to their end. parse_json wants exactly one value there;
  // read_json takes the first and ignores what follows, as `>>` did.
  std::istringstream one("{\"a\":1}\n"), two("{\"a\":1} {\"b\":2}"), two_again("{\"a\":1} {\"b\":2}");
  expect(cgraph::parse_json(one) == nlohmann::json{{"a", 1}}, "parse_json reads a stream");
  expect(cgraph::try_parse_json(two).is_discarded(), "a second value in the stream is an error for parse");
  expect(cgraph::read_json(two_again) == nlohmann::json{{"a", 1}}, "read_json takes the first value");
  std::istringstream too_deep(deep);
  bool refused = false;
  try {
    (void)cgraph::read_json(too_deep);
  } catch (const nlohmann::json::parse_error&) {
    refused = true;
  }
  expect(refused, "read_json refuses past the limit");

  // dump_json: text that is not UTF-8 is written with U+FFFD, valid text as is.
  expect(cgraph::dump_json(nlohmann::json{{"k", "caf\xE9"}}) == "{\"k\":\"caf\xEF\xBF\xBD\"}", "invalid byte replaced");
  expect(cgraph::dump_json(nlohmann::json{{"k", "caf\xC3\xA9"}}) == "{\"k\":\"caf\xC3\xA9\"}", "valid UTF-8 unchanged");
  expect(cgraph::dump_json(nlohmann::json{{"a", {1, 2}}}, 2) == "{\n  \"a\": [\n    1,\n    2\n  ]\n}", "indent is honoured");

  return ok ? 0 : 1;
}
