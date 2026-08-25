// Java interface-dispatch resolution (issue #67). `area` is defined on the
// Shape interface and on both implementations, so pure name resolution had to
// drop `s.area()` as ambiguous and the test could not reach either impl. Java
// reuses `method_declaration` for interface bodies — unlike Go's distinct
// `method_elem` — so the contract is recognized by its OWNING declaration being
// an interface, and `obj.method()` is a member call by the `object` field on
// method_invocation rather than by a member-access wrapper node.
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
  const auto root = std::filesystem::temp_directory_path() / "cgraph_java_dispatch_test";
  std::filesystem::remove_all(root);

  write_file(root / "Shape.java",
             "package demo;\n\npublic interface Shape {\n    double area();\n}\n");
  write_file(root / "Circle.java",
             "package demo;\n\npublic class Circle implements Shape {\n"
             "    public double area() { return 3.0; }\n}\n");
  write_file(root / "Square.java",
             "package demo;\n\npublic class Square implements Shape {\n"
             "    public double area() { return 4.0; }\n}\n");
  write_file(root / "ShapeUserTest.java",
             "package demo;\n\npublic class ShapeUserTest {\n"
             "    public void testArea(Shape s) {\n"
             "        double a = s.area();\n"
             "        s.unpromised();\n"
             "    }\n}\n");
  // An overload set on one type. Making `obj.method()` a member call routes it
  // through the method-only tier, which resolves exactly-one-candidate; without
  // the single-file overload rescue every call to an overloaded method loses
  // its edge (346 of them on stleary/JSON-java, `tokener.nextTo` among them).
  write_file(root / "Tokener.java",
             "package demo;\n\npublic class Tokener {\n"
             "    public String nextTo(char c) { return \"c\"; }\n"
             "    public String nextTo(String s) { return s; }\n}\n");
  write_file(root / "TokenerUserTest.java",
             "package demo;\n\npublic class TokenerUserTest {\n"
             "    public void testNextTo(Tokener t) {\n"
             "        t.nextTo('x');\n"
             "    }\n}\n");

  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;
  std::filesystem::remove_all(root);

  const auto node_in = [&](std::string_view label, std::string_view file_suffix) -> std::string {
    for (const auto& node : graph.nodes) {
      if (node.label == label && node.source_file.ends_with(file_suffix)) {
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

  const auto shape = node_in("Shape", "Shape.java");
  const auto contract_area = node_in("area", "Shape.java");
  const auto circle = node_in("Circle", "Circle.java");
  const auto circle_area = node_in("area", "Circle.java");
  const auto square = node_in("Square", "Square.java");
  const auto square_area = node_in("area", "Square.java");
  const auto test_fn = node_in("testArea", "ShapeUserTest.java");

  // The interface declaration is tagged, which is what distinguishes its
  // method_declaration children from the identically-shaped implementations.
  bool shape_tagged = false;
  for (const auto& node : graph.nodes) {
    if (node.id == shape) {
      const auto tag = node.properties.find("interface");
      shape_tagged = tag != node.properties.end() && tag->second == "true";
    }
  }
  if (shape.empty() || !shape_tagged) {
    return 1;
  }
  // The contract method is owned by the interface.
  if (contract_area.empty() || !edge("method", shape, contract_area)) {
    return 2;
  }
  // Both concrete types satisfy the contract.
  if (!edge("implements", circle, shape) || !edge("implements", square, shape)) {
    return 3;
  }
  // dispatches_to carries the contract to each implementation — exactly two.
  std::size_t dispatch_count = 0;
  for (const auto& e : graph.edges) {
    if (e.relation == "dispatches_to" && e.source == contract_area) {
      ++dispatch_count;
    }
  }
  if (dispatch_count != 2) {
    return 4;
  }
  if (!edge("dispatches_to", contract_area, circle_area) ||
      !edge("dispatches_to", contract_area, square_area)) {
    return 5;
  }
  // The ambiguous member call binds to the contract, so the test reaches the
  // implementations through it. This is the edge issue #67 was filed for.
  if (!edge("CALLS", test_fn, contract_area)) {
    return 6;
  }
  // ...but a member call to a name no interface promises stays dropped, rather
  // than being rescued to some same-named symbol elsewhere in the project.
  for (const auto& e : graph.edges) {
    if (e.relation != "CALLS" || e.source != test_fn) {
      continue;
    }
    for (const auto& node : graph.nodes) {
      if (node.id == e.target && node.label == "unpromised") {
        return 7;
      }
    }
  }

  // An overloaded method reached through a member call edges to EVERY overload
  // (issue #52's rule), rather than dropping because the name is not unique.
  const auto tokener_user = node_in("testNextTo", "TokenerUserTest.java");
  std::size_t overload_edges = 0;
  for (const auto& e : graph.edges) {
    if (e.relation != "CALLS" || e.source != tokener_user) {
      continue;
    }
    for (const auto& node : graph.nodes) {
      if (node.id == e.target && node.label == "nextTo" &&
          node.source_file.ends_with("Tokener.java")) {
        ++overload_edges;
      }
    }
  }
  if (overload_edges != 2) {
    return 8;
  }
  return 0;
}
