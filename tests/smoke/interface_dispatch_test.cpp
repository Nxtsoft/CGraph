// Interface-dispatch resolution: a Go member call whose bare name is ambiguous
// among concrete methods binds to the single interface method promising that
// name, and dispatches_to edges carry it to every implementation — the
// gorilla/mux `matcher.Match` pattern (8 concrete Matches, one contract) that
// pure name-based resolution had to drop as ambiguous.
#include "cgraph/pipeline.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

void write_file(const std::filesystem::path& path, const char* contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_interface_dispatch_test";
  std::filesystem::remove_all(root);

  write_file(root / "match.go",
             "package mux\n\n"
             "type matcher interface {\n\tMatch(s string) bool\n}\n\n"
             "type Route struct{}\n\nfunc (r *Route) Match(s string) bool { return s != \"\" }\n\n"
             "type headerMatcher struct{}\n\nfunc (h headerMatcher) Match(s string) bool { return s == \"h\" }\n");
  write_file(root / "mux_test.go",
             "package mux\n\nimport \"testing\"\n\nfunc TestRoute(t *testing.T) {\n"
             "\tr := &Route{}\n\tif !r.Match(\"x\") {\n\t\tt.Fatal(\"no\")\n\t}\n\tr.Unpromised(\"y\")\n}\n");

  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;
  std::filesystem::remove_all(root);

  const auto id_where = [&](std::string_view label, bool iface) -> std::string {
    for (const auto& node : graph.nodes) {
      const auto tag = node.properties.find("interface_method");
      const bool tagged = tag != node.properties.end() && tag->second == "true";
      if (node.label == label && tagged == iface) {
        return node.id;
      }
    }
    return {};
  };
  const auto id_of = [&](std::string_view label, std::string_view file_suffix) -> std::string {
    for (const auto& node : graph.nodes) {
      if (node.label == label && node.source_file.ends_with(file_suffix) &&
          !node.properties.contains("interface_method")) {
        return node.id;
      }
    }
    return {};
  };
  const auto edge = [&](std::string_view relation, const std::string& source, const std::string& target) {
    return !source.empty() && !target.empty() &&
           std::ranges::any_of(graph.edges, [&](const auto& e) {
             return e.relation == relation && e.source == source && e.target == target;
           });
  };
  const auto edge_from = [&](std::string_view relation, const std::string& source) {
    return !source.empty() && std::ranges::any_of(graph.edges, [&](const auto& e) {
             return e.relation == relation && e.source == source;
           });
  };

  const auto iface_match = id_where("Match", true);
  const auto iface_node = id_of("matcher", "match.go");
  const auto route = id_of("Route", "match.go");
  const auto test_fn = id_of("TestRoute", "mux_test.go");

  // The interface's method set materialized and is owned by the interface.
  if (iface_match.empty() || !edge("method", iface_node, iface_match)) {
    return 1;
  }
  // Both concrete types satisfy the contract.
  if (!edge("implements", route, iface_node)) {
    return 2;
  }
  // dispatches_to carries the contract to each implementation. Two concrete
  // Matches share one label; find them via the edges' existence per type.
  std::size_t dispatch_count = 0;
  for (const auto& e : graph.edges) {
    if (e.relation == "dispatches_to" && e.source == iface_match) {
      ++dispatch_count;
    }
  }
  if (dispatch_count != 2) {
    return 3;
  }
  // The ambiguous member call binds to the contract...
  if (!edge("CALLS", test_fn, iface_match)) {
    return 4;
  }
  // ...and the rescue is real: the test function does have outgoing CALLS.
  if (!edge_from("CALLS", test_fn)) {
    return 5;
  }
  // ...but a member call to a name no interface promises stays dropped:
  // Unpromised(...) resolved nowhere.
  for (const auto& e : graph.edges) {
    if (e.relation != "CALLS" || e.source != test_fn) {
      continue;
    }
    for (const auto& node : graph.nodes) {
      if (node.id == e.target && node.label == "Unpromised") {
        return 6;
      }
    }
  }
  return 0;
}
