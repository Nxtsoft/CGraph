#include "cgraph/pipeline.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

void write_file(const fs::path& path, std::string contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

// True when an edge with the given relation connects the node labelled `source`
// to the node labelled `target`. Matching on labels keeps the test independent
// of the id-normalization scheme.
[[nodiscard]] bool has_edge(const cgraph::GraphSnapshot& graph, const std::string& source,
                            const std::string& target, const std::string& relation) {
  for (const auto& edge : graph.edges) {
    if (edge.relation != relation) {
      continue;
    }
    const cgraph::Node* s = nullptr;
    const cgraph::Node* t = nullptr;
    for (const auto& node : graph.nodes) {
      if (node.id == edge.source) {
        s = &node;
      }
      if (node.id == edge.target) {
        t = &node;
      }
    }
    if (s != nullptr && t != nullptr && s->label == source && t->label == target) {
      return true;
    }
  }
  return false;
}

// Like has_edge but matches file nodes by label suffix, since file-node labels
// are root-relative paths (`.../include/types.hpp`) rather than bare names.
[[nodiscard]] bool has_edge_suffix(const cgraph::GraphSnapshot& graph, const std::string& source_suffix,
                                   const std::string& target_suffix, const std::string& relation) {
  for (const auto& edge : graph.edges) {
    if (edge.relation != relation) {
      continue;
    }
    const cgraph::Node* s = nullptr;
    const cgraph::Node* t = nullptr;
    for (const auto& node : graph.nodes) {
      if (node.id == edge.source) {
        s = &node;
      }
      if (node.id == edge.target) {
        t = &node;
      }
    }
    if (s != nullptr && t != nullptr && s->label.ends_with(source_suffix) && t->label.ends_with(target_suffix)) {
      return true;
    }
  }
  return false;
}

// True when the node whose label starts with `target_prefix` has an incoming
// edge of `relation` from a node of `source_kind`. Prefix + kind matching keeps
// the assertion independent of whether a C++ label carries its signature, so it
// holds both before and after the label change.
[[nodiscard]] bool has_incoming_from_kind(const cgraph::GraphSnapshot& graph,
                                          const std::string& target_prefix,
                                          const std::string& source_kind,
                                          const std::string& relation) {
  for (const auto& edge : graph.edges) {
    if (edge.relation != relation) {
      continue;
    }
    const cgraph::Node* s = nullptr;
    const cgraph::Node* t = nullptr;
    for (const auto& node : graph.nodes) {
      if (node.id == edge.source) {
        s = &node;
      }
      if (node.id == edge.target) {
        t = &node;
      }
    }
    if (s != nullptr && t != nullptr && s->kind == source_kind &&
        t->label.rfind(target_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool has_node(const cgraph::GraphSnapshot& graph, const std::string& label, const std::string& kind) {
  for (const auto& node : graph.nodes) {
    if (node.label == label && node.kind == kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

// End-to-end C++ extraction through the deterministic pipeline: a header declares
// a base struct and a payload type; a .cpp includes it, derives a class with a
// data member and an overriding method, and defines a free function taking the
// payload by reference. Asserts the four relations the C++ extractor adds —
// imports (#include), inherits (base class), references (signature/field types),
// and defines (data members) — all resolve through the include with no dangling.
int main() {
  const auto root = fs::temp_directory_path() / "cgraph_cpp_extractor_test";
  fs::remove_all(root);
  fs::create_directories(root);

  write_file(root / "include" / "types.hpp",
             "#pragma once\n"
             "struct Payload { int value; };\n"
             "struct Base { virtual int run(); };\n");
  write_file(root / "app.cpp",
             "#include \"types.hpp\"\n"
             "\n"
             "struct Mixin {};\n"
             "\n"
             "class Service : public Base, public Mixin {\n"
             "  Payload data;\n"
             " public:\n"
             "  int run() override { return data.value; }\n"
             "};\n"
             "\n"
             "int handle(const Payload& p, Service& s) { return p.value; }\n");
  // A namespace must not become a class node: ids are per-file, so one such node
  // per file all labelled the same never grouped anything, and a class parent
  // makes add_containment_edge label every member a `method`, turning the
  // namespace into a god node that dominates centrality and shortest paths.
  // A C/C++ label must name its symbol, never carry the declaration text. Each
  // line here is a shape that previously produced an unusable label -- or no node
  // at all -- and so kept a bare callee name at a call site from matching.
  write_file(root / "labels.cpp",
             "#include <mutex>\n"
             "std::mutex& ref_return() { static std::mutex m; return m; }\n"
             "int* ptr_return(int x) { return nullptr; }\n"
             "struct Holder {\n"
             "  Holder(int a) {}\n"
             "  ~Holder() {}\n"
             "  bool operator==(const Holder& other) const { return true; }\n"
             "};\n"
             "template <typename T, typename U>\n"
             "T templated(T a, U b) { return a; }\n"
             "int multi_line(\n"
             "    int alpha,\n"
             "    int beta) { return alpha; }\n"
             "int overloaded(int a) { return a; }\n"
             "int overloaded(double a) { return 0; }\n"
             "int caller(int q) { return ptr_return(q) != nullptr ? multi_line(q, q) : overloaded(q); }\n");
  // A callee is keyed on its name too. `obj.f()`, `ptr->f()` and `ns::f()` all
  // used to record the verbatim receiver expression, which matched nothing.
  write_file(root / "callees.cpp",
             "namespace ns {\n"
             "int free_fn(int x) { return x; }\n"
             "struct Svc {\n"
             "  int method(int a) { return a; }\n"
             "  int via_implicit() { return method(1); }\n"
             "};\n"
             "}\n"
             "int callee_user(ns::Svc& s, ns::Svc* p) {\n"
             "  return ns::free_fn(1) + s.method(2) + p->method(3);\n"
             "}\n");
  // A member call must stay scoped to its own file: the receiver type is unknown,
  // so matching a same-named method in another file would invent an edge.
  write_file(root / "elsewhere.cpp",
             "struct Elsewhere {\n"
             "  int only_over_here(int a) { return a; }\n"
             "};\n");
  write_file(root / "reaches.cpp",
             "struct Ptr { Elsewhere* e; };\n"
             "int reaches_across(Ptr& p) { return p.e->only_over_here(5); }\n");
  write_file(root / "scoped.cpp",
             "namespace demo {\n"
             "int scoped_helper(int x) { return x; }\n"
             "struct Holder { int field_value; };\n"
             "}  // namespace demo\n");

  const auto graph = cgraph::run_one_shot(root).graph;

  int failures = 0;
  const auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  };

  // imports: app.cpp -> types.hpp, resolved by include-suffix matching (the
  // header lives under include/, not next to the .cpp).
  check(has_edge_suffix(graph, "app.cpp", "include/types.hpp", "imports"), "import #include types.hpp");

  // inherits: cross-file (Base, in the included header) and same-file (Mixin).
  check(has_edge(graph, "Service", "Base", "inherits"), "inherits cross-file Base");
  check(has_edge(graph, "Service", "Mixin", "inherits"), "inherits same-file Mixin");

  // references: a free function's parameter type and a class data-member type,
  // both resolved to the project type declared in the included header. The
  // source label is the bare name: a C/C++ label names its symbol, it is not the
  // declaration text (this assertion used to read
  // "handle(const Payload& p, Service& s)", which is exactly the shape that kept
  // a bare callee name at a call site from ever matching a declaration).
  check(has_edge(graph, "handle", "Payload", "references"),
        "free-function parameter reference -> Payload");
  check(has_edge(graph, "Service", "Payload", "references"), "field reference -> Payload");

  // defines: a data member becomes a field node owned by its type.
  check(has_node(graph, "data", "field"), "data member node");
  check(has_edge(graph, "Service", "data", "defines"), "defines Service -> data");

  // Labels name symbols. Every shape below used to leak declaration text into
  // the label (or, for the destructor, produce no node at all).
  check(has_node(graph, "ref_return", "function"), "reference return is named, not '& ref_return()'");
  check(has_node(graph, "ptr_return", "function"), "pointer return is named");
  check(has_node(graph, "operator==", "function"), "operator overload is named");
  check(has_node(graph, "templated", "function"), "templated function is named");
  check(has_node(graph, "multi_line", "function"), "multi-line signature is named");
  check(has_node(graph, "~Holder", "function"), "destructor is named");
  // A constructor shares its class's name, so the two must stay distinct nodes
  // rather than colliding on one id.
  check(has_node(graph, "Holder", "class"), "class node");
  check(has_node(graph, "Holder", "function"), "constructor node survives the name clash with its class");
  // An overload set is several symbols, not one: collapsing it deletes real code
  // and leaves an agent one of N answers with no hint the others exist.
  {
    int overloads = 0;
    for (const auto& node : graph.nodes) {
      overloads += (node.kind == "function" && node.label == "overloaded") ? 1 : 0;
    }
    check(overloads == 2, "both overloads survive as distinct nodes");
  }
  // And the point of all of it: a call to a function that takes arguments now
  // resolves. Before, only a zero-argument callee could ever match.
  check(has_edge(graph, "caller", "ptr_return", "CALLS"), "call to a parameterized function resolves");
  check(has_edge(graph, "caller", "multi_line", "CALLS"), "call to a multi-line-signature function resolves");

  // The callee side is keyed on its name too.
  check(has_edge(graph, "callee_user", "free_fn", "CALLS"), "qualified call ns::free_fn resolves");
  check(has_edge(graph, "callee_user", "method", "CALLS"), "member calls obj.f() and ptr->f() resolve");
  check(has_edge(graph, "via_implicit", "method", "CALLS"), "unqualified call to a sibling method resolves");
  // ...but a member call never reaches out of its own file.
  check(!has_edge(graph, "reaches_across", "only_over_here", "CALLS"),
        "member call does not match a same-named method in another file");

  // A namespace is structure, not a type: no class node, no `method` edge, and
  // its members attach to their file with `contains` instead.
  check(!has_node(graph, "demo", "class"), "namespace is not a class node");
  check(has_incoming_from_kind(graph, "scoped_helper", "file", "contains"),
        "namespace member attaches to its file with contains");
  check(!has_incoming_from_kind(graph, "scoped_helper", "class", "method"),
        "no method edge into a namespace member");
  // A real class still owns its methods with `method`, so the fix is targeted.
  check(has_incoming_from_kind(graph, "run", "class", "method"), "class still owns its method");

  // The header's std-free types resolve; nothing dangles.
  for (const auto& edge : graph.edges) {
    bool source_ok = false;
    bool target_ok = false;
    for (const auto& node : graph.nodes) {
      source_ok = source_ok || node.id == edge.source;
      target_ok = target_ok || node.id == edge.target;
    }
    check(source_ok && target_ok, "no dangling edge");
  }

  fs::remove_all(root);
  return failures == 0 ? 0 : 1;
}
