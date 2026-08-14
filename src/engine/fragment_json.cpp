#include "cgraph/fragment_json.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace cgraph {
namespace {

[[nodiscard]] bool require_object(const nlohmann::json& value, std::string_view path, std::vector<std::string>& errors) {
  if (value.is_object()) {
    return true;
  }
  errors.emplace_back(std::string(path) + " must be an object");
  return false;
}

[[nodiscard]] bool require_array(const nlohmann::json& value, std::string_view path, std::vector<std::string>& errors) {
  if (value.is_array()) {
    return true;
  }
  errors.emplace_back(std::string(path) + " must be an array");
  return false;
}

[[nodiscard]] std::string required_string(
    const nlohmann::json& value,
    std::string_view key,
    std::string_view path,
    std::vector<std::string>& errors) {
  const auto iter = value.find(key);
  if (iter == value.end() || !iter->is_string()) {
    errors.emplace_back(std::string(path) + "." + std::string(key) + " must be a string");
    return {};
  }
  return iter->get<std::string>();
}

[[nodiscard]] std::string optional_string(const nlohmann::json& value, std::string_view key) {
  const auto iter = value.find(key);
  if (iter == value.end() || !iter->is_string()) {
    return {};
  }
  return iter->get<std::string>();
}

void parse_confidence_fields(const nlohmann::json& value, Confidence& confidence, std::optional<double>& score) {
  if (const auto iter = value.find("confidence"); iter != value.end() && iter->is_string()) {
    Confidence parsed = Confidence::Extracted;
    if (parse_confidence(iter->get<std::string>(), parsed)) {
      confidence = parsed;
    }
  }

  if (const auto iter = value.find("confidence_score"); iter != value.end() && iter->is_number()) {
    score = iter->get<double>();
  }
}

[[nodiscard]] Properties parse_properties(const nlohmann::json& value) {
  Properties properties;
  const auto iter = value.find("properties");
  if (iter == value.end() || !iter->is_object()) {
    return properties;
  }

  for (const auto& [key, property] : iter->items()) {
    if (property.is_string()) {
      properties.emplace(key, property.get<std::string>());
    } else {
      properties.emplace(key, property.dump());
    }
  }
  return properties;
}

[[nodiscard]] std::optional<SourceLocation> parse_source_location(const nlohmann::json& value) {
  const auto iter = value.find("source_location");
  if (iter == value.end() || !iter->is_object()) {
    return std::nullopt;
  }

  // The rest of the parser tolerates type-confused optional fields by degrading
  // to a default rather than throwing; source_location must do the same.
  // nlohmann's json::value(key, default) THROWS type_error.302 on a type
  // mismatch (a host or LLM writing "start_line": "9"), and no caller on the
  // drop-ingest path catches it -- one such fragment aborted the resident
  // daemon. Accept any non-negative integer, whether nlohmann typed it unsigned
  // (its JSON parser does this for positive literals) or signed; an absent,
  // null, string, bool, float, or negative value degrades to 0 -- and a negative
  // never wraps to a huge unsigned line number, which the old value() coercion
  // did. `any_read` records whether a real number was seen so an all-garbage
  // location does not fabricate a site.
  bool any_read = false;
  const auto number_field = [&iter, &any_read](std::string_view key) -> std::uint32_t {
    const auto field = iter->find(key);
    if (field == iter->end() || !field->is_number_integer()) {
      return 0U;
    }
    const auto raw = field->get<std::int64_t>();
    if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max()) {
      return 0U;
    }
    any_read = true;
    return static_cast<std::uint32_t>(raw);
  };

  SourceLocation location;
  location.start_line = number_field("start_line");
  location.start_column = number_field("start_column");
  location.end_line = number_field("end_line");
  location.end_column = number_field("end_column");

  // With no readable numeric component, treat the location as absent rather than
  // fabricating an all-zero site the host never stated.
  if (!any_read) {
    return std::nullopt;
  }
  return location;
}

[[nodiscard]] Node parse_node(const nlohmann::json& value, std::string_view path, std::vector<std::string>& errors) {
  Node node;
  if (!require_object(value, path, errors)) {
    return node;
  }

  node.id = required_string(value, "id", path, errors);
  node.label = required_string(value, "label", path, errors);
  node.source_file = optional_string(value, "source_file");
  node.source_location = parse_source_location(value);
  node.kind = optional_string(value, "type");
  if (node.kind.empty()) {
    node.kind = optional_string(value, "kind");
  }
  parse_confidence_fields(value, node.confidence, node.confidence_score);
  node.properties = parse_properties(value);
  return node;
}

[[nodiscard]] Edge parse_edge(const nlohmann::json& value, std::string_view path, std::vector<std::string>& errors) {
  Edge edge;
  if (!require_object(value, path, errors)) {
    return edge;
  }

  edge.source = required_string(value, "source", path, errors);
  edge.target = required_string(value, "target", path, errors);
  edge.relation = required_string(value, "relation", path, errors);
  parse_confidence_fields(value, edge.confidence, edge.confidence_score);
  edge.properties = parse_properties(value);
  return edge;
}

[[nodiscard]] Hyperedge parse_hyperedge(const nlohmann::json& value, std::string_view path, std::vector<std::string>& errors) {
  Hyperedge hyperedge;
  if (!require_object(value, path, errors)) {
    return hyperedge;
  }

  hyperedge.id = required_string(value, "id", path, errors);
  hyperedge.relation = required_string(value, "relation", path, errors);

  const auto nodes = value.find("nodes");
  if (nodes == value.end() || !nodes->is_array()) {
    errors.emplace_back(std::string(path) + ".nodes must be an array");
  } else {
    for (const auto& node : *nodes) {
      if (node.is_string()) {
        hyperedge.nodes.push_back(node.get<std::string>());
      } else {
        errors.emplace_back(std::string(path) + ".nodes entries must be strings");
      }
    }
  }

  parse_confidence_fields(value, hyperedge.confidence, hyperedge.confidence_score);
  hyperedge.properties = parse_properties(value);
  return hyperedge;
}

}  // namespace

std::string confidence_to_string(Confidence confidence) {
  switch (confidence) {
    case Confidence::Extracted:
      return "EXTRACTED";
    case Confidence::Inferred:
      return "INFERRED";
    case Confidence::Ambiguous:
      return "AMBIGUOUS";
  }
  return "AMBIGUOUS";
}

bool parse_confidence(std::string_view value, Confidence& confidence) {
  if (value == "EXTRACTED") {
    confidence = Confidence::Extracted;
    return true;
  }
  if (value == "INFERRED") {
    confidence = Confidence::Inferred;
    return true;
  }
  if (value == "AMBIGUOUS") {
    confidence = Confidence::Ambiguous;
    return true;
  }
  return false;
}

nlohmann::json to_json(const SourceLocation& location) {
  return nlohmann::json{
      {"start_line", location.start_line},
      {"start_column", location.start_column},
      {"end_line", location.end_line},
      {"end_column", location.end_column},
  };
}

nlohmann::json to_json(const Node& node) {
  auto value = nlohmann::json{
      {"id", node.id},
      {"label", node.label},
      {"source_file", node.source_file},
      {"type", node.kind},
      {"confidence", confidence_to_string(node.confidence)},
      {"properties", node.properties},
  };
  if (node.source_location.has_value()) {
    value["source_location"] = to_json(*node.source_location);
  }
  if (node.confidence_score.has_value()) {
    value["confidence_score"] = *node.confidence_score;
  }
  return value;
}

nlohmann::json to_json(const Edge& edge) {
  auto value = nlohmann::json{
      {"source", edge.source},
      {"target", edge.target},
      {"relation", edge.relation},
      {"confidence", confidence_to_string(edge.confidence)},
      {"properties", edge.properties},
  };
  if (edge.confidence_score.has_value()) {
    value["confidence_score"] = *edge.confidence_score;
  }
  return value;
}

nlohmann::json to_json(const Hyperedge& hyperedge) {
  auto value = nlohmann::json{
      {"id", hyperedge.id},
      {"nodes", hyperedge.nodes},
      {"relation", hyperedge.relation},
      {"confidence", confidence_to_string(hyperedge.confidence)},
      {"properties", hyperedge.properties},
  };
  if (hyperedge.confidence_score.has_value()) {
    value["confidence_score"] = *hyperedge.confidence_score;
  }
  return value;
}

nlohmann::json to_json(const Fragment& fragment) {
  auto nodes = nlohmann::json::array();
  for (const auto& node : fragment.nodes) {
    nodes.push_back(to_json(node));
  }

  auto edges = nlohmann::json::array();
  for (const auto& edge : fragment.edges) {
    edges.push_back(to_json(edge));
  }

  auto hyperedges = nlohmann::json::array();
  for (const auto& hyperedge : fragment.hyperedges) {
    hyperedges.push_back(to_json(hyperedge));
  }

  return nlohmann::json{
      {"nodes", std::move(nodes)},
      {"edges", std::move(edges)},
      {"hyperedges", std::move(hyperedges)},
      {"warnings", fragment.warnings},
  };
}

bool parse_fragment(const nlohmann::json& value, Fragment& fragment, std::vector<std::string>& errors) {
  fragment = Fragment{};
  errors.clear();

  if (!require_object(value, "$", errors)) {
    return false;
  }

  const auto nodes = value.find("nodes");
  if (nodes == value.end()) {
    errors.emplace_back("$.nodes must be an array");
  } else if (require_array(*nodes, "$.nodes", errors)) {
    for (std::size_t index = 0; index < nodes->size(); ++index) {
      fragment.nodes.push_back(parse_node((*nodes)[index], "$.nodes[" + std::to_string(index) + "]", errors));
    }
  }

  const auto edges = value.find("edges");
  if (edges == value.end()) {
    errors.emplace_back("$.edges must be an array");
  } else if (require_array(*edges, "$.edges", errors)) {
    for (std::size_t index = 0; index < edges->size(); ++index) {
      fragment.edges.push_back(parse_edge((*edges)[index], "$.edges[" + std::to_string(index) + "]", errors));
    }
  }

  const auto hyperedges = value.find("hyperedges");
  if (hyperedges != value.end() && require_array(*hyperedges, "$.hyperedges", errors)) {
    for (std::size_t index = 0; index < hyperedges->size(); ++index) {
      fragment.hyperedges.push_back(parse_hyperedge((*hyperedges)[index], "$.hyperedges[" + std::to_string(index) + "]", errors));
    }
  }

  if (const auto warnings = value.find("warnings"); warnings != value.end() && warnings->is_array()) {
    for (const auto& warning : *warnings) {
      if (warning.is_string()) {
        fragment.warnings.push_back(warning.get<std::string>());
      }
    }
  }

  return errors.empty();
}

}  // namespace cgraph
