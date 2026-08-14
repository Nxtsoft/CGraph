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
  // mistyped fields degrade and the one well-typed field (end_column: 4) is
  // honoured, so the location is present and reflects only that field.
  if (!cgraph::parse_fragment(poison, poisoned, errors)) {
    return 1;
  }
  if (poisoned.nodes.size() != 1) {
    return 1;
  }
  {
    const auto& loc = poisoned.nodes[0].source_location;
    if (!loc.has_value()) {
      return 1;
    }
    if (loc->start_line != 0 || loc->start_column != 0 || loc->end_line != 0 || loc->end_column != 4) {
      return 1;
    }
  }

  // A location whose every component is unreadable must be ABSENT, not a
  // fabricated line-0/column-0 site the host never stated.
  const auto all_garbage = nlohmann::json{
      {"nodes",
       nlohmann::json::array({
           {
               {"id", "g"},
               {"label", "G"},
               {"type", "document"},
               {"source_location",
                {
                    {"start_line", "9"},
                    {"start_column", "1"},
                    {"end_line", "2"},
                    {"end_column", "3"},
                }},
           },
       })},
      {"edges", nlohmann::json::array()},
  };
  cgraph::Fragment garbage;
  if (!cgraph::parse_fragment(all_garbage, garbage, errors)) {
    return 1;
  }
  if (garbage.nodes.size() != 1 || garbage.nodes[0].source_location.has_value()) {
    return 1;
  }

  // A negative or above-uint32 component degrades to 0, never wraps.
  const auto out_of_range = nlohmann::json{
      {"nodes",
       nlohmann::json::array({
           {
               {"id", "r"},
               {"label", "R"},
               {"source_location",
                {
                    {"start_line", -5},
                    {"end_line", 4294967296},
                }},
           },
       })},
      {"edges", nlohmann::json::array()},
  };
  cgraph::Fragment ranged;
  if (!cgraph::parse_fragment(out_of_range, ranged, errors)) {
    return 1;
  }
  // Neither component is readable as a uint32 (negative / oversized), so the
  // location has no readable component and is absent.
  if (ranged.nodes[0].source_location.has_value()) {
    return 1;
  }

  return 0;
}
