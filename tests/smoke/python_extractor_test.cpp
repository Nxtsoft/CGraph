#include "cgraph/python_extractor.hpp"
#include "cgraph/normalize.hpp"
#include <set>

int main() {
  const auto members = cgraph::extract_python({.source_file = "members.py", .source = R"py(
from dataclasses import dataclass
@dataclass
class First:
    size: int
    name: str = ""
    def method(self):
        local: int = 0
class Second:
    size: int
    name: str
)py"});
  for (const std::string owner : {"First", "Second"}) {
    std::set<std::string> labels;
    for (const auto& edge : members.fragment.edges) {
      if (edge.relation != "defines" || edge.source != cgraph::make_id("members.py:" + owner)) continue;
      for (const auto& field : members.fragment.nodes) {
        if (field.id != edge.target) continue;
        if (field.kind != "field" || field.id != cgraph::make_id("members.py:" + owner + "::" + field.label)) return 1;
        if (field.properties.at("type_text") != (field.label == "size" ? "int" : "str")) return 1;
        labels.insert(field.label);
      }
    }
    if (labels != std::set<std::string>{"size", "name"}) return 1;
  }

  // make_id collapses `First::size` and `first_size` onto one id
  // (normalize.cpp), and merge_fragments keeps only the first node with an id.
  // The function must keep the plain id an agent queries it by, and the field
  // must still exist and be the target of its owner's `defines` edge.
  const auto collision = cgraph::extract_python({.source_file = "c.py", .source = R"py(
def first_size():
    return 1
class First:
    size: int
)py"});
  const cgraph::Node* collided_function = nullptr;
  const cgraph::Node* collided_field = nullptr;
  for (const auto& node : collision.fragment.nodes) {
    if (node.kind == "function" && node.label == "first_size") collided_function = &node;
    if (node.kind == "field" && node.label == "size") collided_field = &node;
  }
  if (collided_function == nullptr || collided_field == nullptr) return 1;
  if (collided_function->id != cgraph::make_id("c.py:first_size")) return 1;
  if (collided_field->id == collided_function->id) return 1;
  bool defines_the_field = false;
  for (const auto& edge : collision.fragment.edges) {
    if (edge.relation != "defines" || edge.source != cgraph::make_id("c.py:First")) continue;
    if (edge.target == collided_function->id) return 1;
    defines_the_field = defines_the_field || edge.target == collided_field->id;
  }
  if (!defines_the_field) return 1;

  constexpr auto source = R"py(
import os
from pathlib import Path

class Worker:
    def run(self):
        return Path.cwd()
)py";

  const auto result = cgraph::extract_python(
      cgraph::ExtractionContext{.source_file = "worker.py", .source = source});

  if (result.fragment.nodes.size() < 4) {
    return 1;
  }
  if (result.raw_calls.empty()) {
    return 1;
  }

  bool saw_import = false;
  for (const auto& node : result.fragment.nodes) {
    if (node.kind == "import") {
      saw_import = true;
    }
  }
  if (!saw_import) {
    return 1;
  }

  return 0;
}
