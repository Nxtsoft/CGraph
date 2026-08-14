#include "cgraph/fragment_json.hpp"

#include <nlohmann/json.hpp>

#include <vector>

int main() {
  const auto input = nlohmann::json{
      {"nodes",
       nlohmann::json::array({
           {
               {"id", "foo"},
               {"label", "Foo"},
               {"source_file", "src/foo.cpp"},
               {"source_location",
                {
                    {"start_line", 1},
                    {"start_column", 2},
                    {"end_line", 3},
                    {"end_column", 4},
                }},
               {"type", "function"},
               {"confidence", "EXTRACTED"},
           },
       })},
      {"edges",
       nlohmann::json::array({
           {
               {"source", "foo"},
               {"target", "bar"},
               {"relation", "CALLS"},
               {"confidence", "INFERRED"},
               {"confidence_score", 0.75},
           },
       })},
      {"hyperedges", nlohmann::json::array()},
  };

  cgraph::Fragment fragment;
  std::vector<std::string> errors;
  if (!cgraph::parse_fragment(input, fragment, errors)) {
    return 1;
  }
  if (fragment.nodes.size() != 1 || fragment.edges.size() != 1) {
    return 1;
  }
  if (fragment.nodes[0].id != "foo" || fragment.edges[0].confidence != cgraph::Confidence::Inferred) {
    return 1;
  }

  const auto output = cgraph::to_json(fragment);
  if (output["nodes"][0]["confidence"] != "EXTRACTED") {
    return 1;
  }
  if (output["edges"][0]["confidence"] != "INFERRED") {
    return 1;
  }

  const auto invalid = nlohmann::json{
      {"nodes", nlohmann::json::array({{{"id", "missing-label"}}})},
      {"edges", nlohmann::json::array({{{"source", "foo"}, {"target", "bar"}}})},
  };
  if (cgraph::parse_fragment(invalid, fragment, errors)) {
    return 1;
  }
  if (errors.empty()) {
    return 1;
  }

  // A type-confused source_location field must degrade to absent, never throw.
  // A host (or LLM) writing "start_line": "9" -- a string where an int is
  // expected -- once crashed the resident daemon with an uncaught
  // nlohmann::type_error: the whole optional-field contract is "tolerate garbage,
  // reject only on structure", and every other optional field honours it. A
  // parse must be total: no fragment shape may throw out of parse_fragment.
  const auto poison = nlohmann::json{
      {"nodes",
       nlohmann::json::array({
           {
               {"id", "poison"},
               {"label", "Poison"},
               {"type", "document"},
               {"source_file", "docs/notes.md"},
               {"source_location",
                {
                    {"start_line", "9"},        // string, not number
                    {"start_column", nullptr},  // null
                    {"end_line", true},         // bool
                    {"end_column", 4},          // the one well-typed field
                }},
           },
       })},
      {"edges", nlohmann::json::array()},
  };
  cgraph::Fragment poisoned;
  // Must not throw. The fragment is structurally valid, so it parses; the
  // mistyped location fields degrade -- absent location, or well-typed fields
  // honoured and mistyped ones defaulted -- never an abort.
  if (!cgraph::parse_fragment(poison, poisoned, errors)) {
    return 1;
  }
  if (poisoned.nodes.size() != 1) {
    return 1;
  }
  // A location whose components could not be read as numbers must not silently
  // become line 0 col 0 -- an int-typed absent field is a fabricated site. With
  // no readable numeric component the location is absent.
  if (poisoned.nodes[0].source_location.has_value() &&
      poisoned.nodes[0].source_location->start_line == 0 &&
      poisoned.nodes[0].source_location->start_column == 0 &&
      poisoned.nodes[0].source_location->end_line == 0) {
    // end_column was 4, so a present location is acceptable only if it reflects
    // that; an all-zero location paired with the string/null/bool fields is the
    // fabricated-site bug.
    if (poisoned.nodes[0].source_location->end_column != 4) {
      return 1;
    }
  }

  return 0;
}
