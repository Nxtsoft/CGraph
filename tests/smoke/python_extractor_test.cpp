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
