// Receiver-scoped member-call resolution. `Alpha.parse(s)` names the class that
// owns the method, so the call must bind to Alpha's `parse` even though other
// files declare a `parse` too — a project-wide name lookup drops that as
// ambiguous and the caller reaches nothing. This is the stleary/JSON-java
// pattern: `toJSONObject` is declared in eight files, so every single
// `XML.toJSONObject(...)` call in XMLTest.java produced no edge at all.
//
// The receiver must match the declaration's name EXACTLY. make_id folds case,
// which would also bind an instance named after its type; that is a convention,
// not proof, and it would equally bind an unrelated same-named variable.
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
  const auto root = std::filesystem::temp_directory_path() / "cgraph_java_receiver_scope_test";
  std::filesystem::remove_all(root);

  // `parse` is declared in two files, and overloaded in one of them.
  write_file(root / "Alpha.java",
             "package demo;\n\npublic class Alpha {\n"
             "    public static String parse(String s) { return s; }\n"
             "    public static String parse(String s, boolean b) { return s; }\n}\n");
  write_file(root / "Beta.java",
             "package demo;\n\npublic class Beta {\n"
             "    public static String parse(String s) { return s; }\n}\n");
  // A lowercase instance whose name differs from its type only by case. The
  // case-folded form would bind this to class Alpha; exact-case must not.
  write_file(root / "UserTest.java",
             "package demo;\n\npublic class UserTest {\n"
             "    public void testStatic() {\n"
             "        String r = Alpha.parse(\"x\");\n"
             "    }\n"
             "    public void testConventionNamed(Alpha alpha) {\n"
             "        String r = alpha.parse(\"y\");\n"
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
  const auto calls_from = [&](const std::string& source, std::string_view file_suffix) {
    std::size_t count = 0;
    for (const auto& e : graph.edges) {
      if (e.relation != "CALLS" || e.source != source) {
        continue;
      }
      for (const auto& node : graph.nodes) {
        if (node.id == e.target && node.label == "parse" &&
            node.source_file.ends_with(file_suffix)) {
          ++count;
        }
      }
    }
    return count;
  };

  const auto test_static = node_in("testStatic", "UserTest.java");
  if (test_static.empty()) {
    return 1;
  }
  // `Alpha.parse(...)` reaches BOTH of Alpha's overloads — without types any
  // member may be the callee (issue #52's rule), applied within the class.
  if (calls_from(test_static, "Alpha.java") != 2) {
    return 2;
  }
  // ...and never Beta's, which the receiver excluded. A project-wide lookup
  // would have found three candidates and dropped the call entirely.
  if (calls_from(test_static, "Beta.java") != 0) {
    return 3;
  }

  // The case-folded receiver stays unresolved: `alpha` is not the declaration's
  // name, so this tier declines rather than guessing by convention.
  const auto test_conv = node_in("testConventionNamed", "UserTest.java");
  if (test_conv.empty()) {
    return 4;
  }
  if (calls_from(test_conv, "Alpha.java") != 0) {
    return 5;
  }
  return 0;
}
