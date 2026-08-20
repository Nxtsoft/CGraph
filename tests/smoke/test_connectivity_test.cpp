// Regression tests for issues #44, #45, and #58: tests must be able to reach
// the code they exercise.
//
// #44 (Go): receiver-method calls (`r.Match(...)`) resolved same-file only, so
// a method had zero cross-file incoming CALLS — on gorilla/mux, route.go's 53
// symbols had none. Member calls now resolve project-wide when the bare name
// uniquely names a METHOD (never a free function), graded INFERRED.
//
// #45 (Python): the import handler emitted a dead-end node (whole statement as
// label, no import_path, no edges) so Python graphs carried no import
// relations at all, and `obj.method()` calls were unresolvable. Imports now
// emit the standard module/import stub shape, `__init__.py` anchors its
// package directory, and `attribute` calls are member calls.
//
// #58 (Rust): calls inside macro invocations were never extracted (assertions
// are macros, so most test-body calls did not exist), impl methods carried no
// method membership so `c.bump()` resolved to nothing, and the extern-crate
// `use` spelling — the only one an integration test can use — produced no
// import edge, nor did `pub mod x;`. Across six public repos test-file
// reachability measured 0.000–0.142 vs 0.955 on a healthy TS graph.
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

struct Ids {
  const cgraph::GraphSnapshot& graph;
  [[nodiscard]] std::string id_of(std::string_view label, std::string_view file_suffix) const {
    for (const auto& node : graph.nodes) {
      if (node.label == label && node.source_file.ends_with(file_suffix)) {
        return node.id;
      }
    }
    return {};
  }
  [[nodiscard]] bool edge(std::string_view relation, const std::string& source, const std::string& target) const {
    return !source.empty() && !target.empty() &&
           std::ranges::any_of(graph.edges, [&](const auto& e) {
             return e.relation == relation && e.source == source && e.target == target;
           });
  }
};

}  // namespace

int main() {
  namespace fs = std::filesystem;

  // --- Go: cross-file receiver-method call resolves to the method (#44) ---
  {
    const auto root = fs::temp_directory_path() / "cgraph_go_member_test";
    fs::remove_all(root);
    write_file(root / "route.go",
               "package mux\n\ntype Route struct{}\n\nfunc (r *Route) Match(s string) bool {\n\treturn s != \"\"\n}\n\n"
               "func Free(s string) bool {\n\treturn s == \"\"\n}\n");
    write_file(root / "mux_test.go",
               "package mux\n\nimport \"testing\"\n\nfunc TestMatch(t *testing.T) {\n\tr := &Route{}\n"
               "\tif !r.Match(\"x\") {\n\t\tt.Fatal(\"no\")\n\t}\n\tr.Free(\"y\")\n}\n");
    const auto result = cgraph::run_one_shot(root);
    const Ids ids{result.graph};
    fs::remove_all(root);

    const auto test_fn = ids.id_of("TestMatch", "mux_test.go");
    const auto method = ids.id_of("Match", "route.go");
    const auto free_fn = ids.id_of("Free", "route.go");
    // The member call binds to the method across files...
    if (!ids.edge("CALLS", test_fn, method)) {
      return 1;
    }
    // ...but a member call must never bind to a free function of the same name.
    if (ids.edge("CALLS", test_fn, free_fn)) {
      return 2;
    }
  }

  // --- Python: package imports resolve; method calls bind cross-file (#45) ---
  {
    const auto root = fs::temp_directory_path() / "cgraph_py_import_test";
    fs::remove_all(root);
    write_file(root / "src/pkg/__init__.py", "from .signer import Signer\n");
    write_file(root / "src/pkg/signer.py",
               "class Signer:\n    def sign(self, value):\n        return value\n");
    write_file(root / "tests/test_signer.py",
               "from pkg import Signer\n\ndef test_sign():\n    s = Signer()\n    assert s.sign(\"a\") == \"a\"\n");
    const auto result = cgraph::run_one_shot(root);
    const Ids ids{result.graph};
    fs::remove_all(root);

    const auto init_file = ids.id_of("pkg/__init__.py", "__init__.py");
    const auto signer_file = ids.id_of("pkg/signer.py", "signer.py");
    const auto test_file = ids.id_of("tests/test_signer.py", "test_signer.py");
    const auto sign_method = ids.id_of("sign", "signer.py");
    const auto test_fn = ids.id_of("test_sign", "test_signer.py");

    // `from pkg import Signer` anchors the package directory to __init__.py.
    if (!ids.edge("imports_from", test_file, init_file)) {
      return 3;
    }
    // __init__'s relative `from .signer import ...` reaches the module.
    if (!ids.edge("imports_from", init_file, signer_file)) {
      return 4;
    }
    // `s.sign(...)` binds to the method across files.
    if (!ids.edge("CALLS", test_fn, sign_method)) {
      return 5;
    }
    // No unresolved stub may remain.
    if (std::ranges::any_of(result.graph.nodes,
                            [](const auto& n) { return n.kind == "module" || n.kind == "import"; })) {
      return 6;
    }
  }

  // --- Rust: macro-wrapped calls, impl-method member calls, extern-crate
  //     imports, and `mod` declarations connect tests to code (#58) ---
  {
    const auto root = fs::temp_directory_path() / "cgraph_rust_connectivity_test";
    fs::remove_all(root);
    write_file(root / "repro2/Cargo.toml",
               "[package]\nname = \"repro2\"\nversion = \"0.1.0\"\nedition = \"2021\"\n");
    write_file(root / "repro2/src/lib.rs",
               "pub mod math;\n\npub struct Counter {\n    pub n: i32,\n}\n\nimpl Counter {\n"
               "    pub fn new() -> Self {\n        Counter { n: 0 }\n    }\n"
               "    pub fn bump(&mut self) -> i32 {\n        self.n += 1;\n        self.n\n    }\n}\n");
    write_file(root / "repro2/src/math.rs",
               "pub fn add(a: i32, b: i32) -> i32 {\n    a + b\n}\n");
    write_file(root / "repro2/tests/only_macro.rs",
               "use repro2::math::add;\n\n#[test]\nfn only_call_is_macro_wrapped() {\n"
               "    assert_eq!(add(1, 2), 3);\n}\n");
    write_file(root / "repro2/tests/only_method.rs",
               "use repro2::Counter;\n\n#[test]\nfn only_call_is_a_method() {\n"
               "    let mut c = Counter::new();\n    let n = c.bump();\n    let _ = n;\n}\n\n"
               // A test fn named after the method it tests (tokio's convention:
               // `fn blocking_acquire()` testing `sem.blocking_acquire()`). The
               // same-file name match is the sibling test, not the method — the
               // member call must skip it and still reach the impl method.
               "#[test]\nfn bump() {\n    let mut c = Counter::new();\n"
               "    assert_eq!(c.bump(), 1);\n}\n");
    const auto result = cgraph::run_one_shot(root);
    const Ids ids{result.graph};
    fs::remove_all(root);

    const auto macro_test = ids.id_of("only_call_is_macro_wrapped", "only_macro.rs");
    const auto method_test = ids.id_of("only_call_is_a_method", "only_method.rs");
    const auto add_fn = ids.id_of("add", "math.rs");
    const auto bump = ids.id_of("bump", "lib.rs");
    const auto lib_file = ids.id_of("src/lib.rs", "lib.rs");
    const auto math_file = ids.id_of("src/math.rs", "math.rs");
    const auto macro_file = ids.id_of("tests/only_macro.rs", "only_macro.rs");

    // The only call in the test body sits inside assert_eq! — it must exist
    // and bind across files.
    if (!ids.edge("CALLS", macro_test, add_fn)) {
      return 7;
    }
    // `c.bump()` binds to the impl method across files (the Rust twin of #44).
    if (!ids.edge("CALLS", method_test, bump)) {
      return 8;
    }
    // `use repro2::math::add` resolves through the package name to the item.
    if (!ids.edge("imports", macro_file, add_fn)) {
      return 9;
    }
    // `pub mod math;` ties the module file into the tree.
    if (!ids.edge("imports", lib_file, math_file)) {
      return 10;
    }
    // The test fn named after the method: its member call skips the same-file
    // name match (itself) and binds to the impl method cross-file.
    const auto bump_test = ids.id_of("bump", "only_method.rs");
    if (!ids.edge("CALLS", bump_test, bump)) {
      return 11;
    }
    // And the sibling test's member call must not bind to the same-named test fn.
    if (ids.edge("CALLS", method_test, bump_test)) {
      return 12;
    }
  }
  return 0;
}
