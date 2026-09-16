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

// Like has_edge, and the edge must also carry the given `context` property.
[[nodiscard]] bool has_edge_with_context(const cgraph::GraphSnapshot& graph, const std::string& source,
                                         const std::string& target, const std::string& relation,
                                         const std::string& context) {
  for (const auto& edge : graph.edges) {
    if (edge.relation != relation) {
      continue;
    }
    const auto ctx = edge.properties.find("context");
    if (ctx == edge.properties.end() || ctx->second != context) {
      continue;
    }
    bool source_ok = false;
    bool target_ok = false;
    for (const auto& node : graph.nodes) {
      if (node.id == edge.source && node.label == source) {
        source_ok = true;
      }
      if (node.id == edge.target && node.label == target) {
        target_ok = true;
      }
    }
    if (source_ok && target_ok) {
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
  // A header that includes the header: transitive include resolution reaches
  // Payload from a file that never names types.hpp itself.
  write_file(root / "include" / "engine.hpp",
             "#pragma once\n"
             "#include \"types.hpp\"\n"
             "struct Engine { int start(); };\n");
  write_file(root / "consumer.cpp",
             "#include \"engine.hpp\"\n"
             "int consume(const Payload& p, Engine& e) { return p.value; }\n");
  // Template arguments of namespace-qualified types are references too (#94):
  // std::vector<Payload>&, std::span<const Payload>, std::optional<Payload>.
  write_file(root / "generics.cpp",
             "#include \"types.hpp\"\n"
             "#include <optional>\n"
             "#include <span>\n"
             "#include <vector>\n"
             "std::optional<Payload> maybe_payload(std::vector<Payload>& all, std::span<const Payload> view) {\n"
             "  return all.empty() ? std::nullopt : std::optional<Payload>{all.front()};\n"
             "}\n"
             "struct PayloadBag { std::vector<Payload> items; };\n");
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
  // A template instantiation's callee text also contains `::`, so a blind tail
  // reduction turns `wrapper<zoo::Beast>` into `Beast>` -- which make_id
  // normalizes to `Beast`, inventing a call to an unrelated struct. And a LEADING
  // `::` is explicit global scope, so `::stat(...)` must not resolve to a local
  // struct of that name.
  write_file(root / "qualify.cpp",
             "namespace zoo { struct Beast { int n; }; }\n"
             "namespace ns {\n"
             "  template <typename T> int made(int a) { return a; }\n"
             "  int plain(int a) { return a; }\n"
             "}\n"
             "template <typename T> int wrapper(int a) { return a; }\n"
             "int only_real_call(int a) { return a; }\n"
             "struct stat_local { int st; };\n"
             "int probe(const char* p) { return ::stat_local_probe(p); }\n"
             "int drive() {\n"
             "  return wrapper<zoo::Beast>(1) + ns::made<zoo::Beast>(2)\n"
             "       + ns::plain(3) + only_real_call(4);\n"
             "}\n");
  // Three overloads can share ONE line, so a single id retry is not enough.
  write_file(root / "sameline.cpp",
             "int triple(int a) { return a; } int triple(double a) { return 0; } int triple(char a) { return 1; }\n");
  // A member call that misses its own file resolves project-wide only when the
  // bare name uniquely names a METHOD (issue #44): `only_over_here` is declared
  // exactly once, as a method, so the edge is real (graded INFERRED). A name
  // also carried by a free function must never bind (`freestanding` below), and
  // an ambiguous method name must not either.
  write_file(root / "elsewhere.cpp",
             "struct Elsewhere {\n"
             "  int only_over_here(int a) { return a; }\n"
             "};\n"
             "int freestanding(int a) { return a; }\n");
  write_file(root / "reaches.cpp",
             "struct Ptr { Elsewhere* e; };\n"
             "int reaches_across(Ptr& p) { return p.e->only_over_here(5) + p.e->freestanding(6); }\n");
  write_file(root / "twins.cpp",
             "struct TwinA { int twin_method(); };\n"
             "struct TwinB { int twin_method(); };\n"
             "int calls_twin(TwinA& a) { return a.twin_method(); }\n");
  write_file(root / "scoped.cpp",
             "namespace demo {\n"
             "int scoped_helper(int x) { return x; }\n"
             "struct Holder { int field_value; };\n"
             "}  // namespace demo\n");

  // A qualified callee's scope is evidence. `std::find` reduces to the leaf
  // `find`, which names exactly one project function -- but that function is
  // declared in `proj`, not `std`, so the call must not bind (every call into
  // the standard library used to become a dependent of a same-named project
  // symbol). `proj::helper` and the class-qualified `proj::Stats::size_of` bind.
  write_file(root / "stdlib_decls.hpp",
             "#pragma once\n"
             "#include <vector>\n"
             "namespace proj {\n"
             "int find(int x) { return x; }\n"
             "int exists(int x) { return x; }\n"
             "struct Stats { static int size_of() { return 1; } int size() const { return 0; }\n"
             "               static int count_of(std::vector<int>& v) { return (int)v.size(); } };\n"
             "namespace detail { int helper() { return 2; } }\n"
             "}\n");
  // The caller lives in another file: `v.size()` on an unknown receiver must not
  // reach the project's only method named `size` (same-file binding is a
  // different tier and stays), and `std::find` must not reach `proj::find`.
  write_file(root / "stdlib_user.cpp",
             "#include \"stdlib_decls.hpp\"\n"
             "#include <algorithm>\n"
             "#include <filesystem>\n"
             "#include <vector>\n"
             "int stdlib_user(std::vector<int>& v) {\n"
             "  auto it = std::find(v.begin(), v.end(), 3);\n"
             "  auto n = v.size() + proj::Stats::count_of(v);\n"
             "  bool there = std::filesystem::exists(\"x\");\n"
             "  return (it != v.end()) + there + proj::detail::helper() + proj::Stats::size_of() + n;\n"
             "}\n");
  // The qualifier is reasoned about in segments, through real grammar shapes:
  // a static call through a class template, a qualified call to a symbol in an
  // anonymous namespace, and an overload set split across a namespace and file
  // scope with the file-scope declaration first.
  write_file(root / "scope_shapes.hpp",
             "#pragma once\n"
             "namespace proj {\n"
             "struct Beast { int n; };\n"
             "template <typename T> struct Outer { static int make() { return 1; } };\n"
             "namespace { int hidden() { return 3; } }\n"
             "int hidden_user() { return proj::hidden(); }\n"
             "}\n"
             "int dup(int a) { return a; }\n"
             "namespace alpha { int dup(double a) { return 0; } }\n");
  write_file(root / "scope_shapes.cpp",
             "#include \"scope_shapes.hpp\"\n"
             "int template_user() { return proj::Outer<int>::make() + proj::Outer<proj::Beast>::make(); }\n"
             "int alpha_user() { return alpha::dup(1.0); }\n");

  // A qualifier refuses a candidate only where it CONTRADICTS what that
  // candidate records. Five of these shapes resolved by name and were then
  // dropped as `dropped_scope_mismatch`; the sixth, an in-class definition, is
  // the control that bound all along (CGR-4 review of #76).
  write_file(root / "scope_evidence.hpp",
             "#pragma once\n"
             "namespace proj {\n"
             "struct Cache { static int reload(); };\n"
             "struct Nest { struct Inner { static int spin(); }; };\n"
             "struct Inline { static int inline_reload() { return 7; } };\n"
             "namespace detail { int helper_decl() { return 3; } }\n"
             "inline namespace v1 { int versioned() { return 8; } }\n"
             "}\n");
  // Out-of-line member definitions: the class qualifier is on the DEFINITION,
  // never on the in-class prototype (a field_declaration, which gets no node).
  write_file(root / "scope_evidence.cpp",
             "#include \"scope_evidence.hpp\"\n"
             "namespace proj {\n"
             "int Cache::reload() { return 1; }\n"
             "int Nest::Inner::spin() { return 2; }\n"
             "}\n");
  write_file(root / "scope_evidence_user.cpp",
             "#include \"scope_evidence.hpp\"\n"
             "namespace pd = proj::detail;\n"
             "template <typename T> int build() { return T::make(); }\n"
             "int user_outofline() { return proj::Cache::reload(); }\n"
             "int user_nested() { return proj::Nest::Inner::spin(); }\n"
             "int via_alias() { return pd::helper_decl(); }\n"
             "int via_inline_ns() { return proj::versioned(); }\n"
             "int user_inline() { return proj::Inline::inline_reload(); }\n");

  // Library namespaces the project declares nowhere, against file-scope project
  // functions of the same name. While the contradiction test was an allowlist
  // holding `std` alone, all four of these bound (CGR-4 review of #80).
  write_file(root / "library_roots.cpp",
             "int format(int a) { return a; }\n"
             "int trim(int a) { return a; }\n"
             "int capacity() { return 0; }\n"
             "int number(int a) { return a; }\n");
  write_file(root / "library_roots_user.cpp",
             "int library_user(int a) {\n"
             "  return fmt::format(a) + boost::algorithm::trim(a) + absl::strings_internal::capacity() +\n"
             "         QString::number(a);\n"
             "}\n");

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
  // A template argument of a qualified type is a reference, tagged generic_arg,
  // whether it sits in a parameter, a return type or a data member.
  check(has_edge_with_context(graph, "maybe_payload", "Payload", "references", "generic_arg"),
        "std::vector<Payload>& / std::span<const Payload> / std::optional<Payload> -> Payload as generic_arg");
  check(has_edge_with_context(graph, "PayloadBag", "Payload", "references", "generic_arg"),
        "std::vector<Payload> data member -> Payload as generic_arg");
  // consumer.cpp includes engine.hpp, which includes types.hpp: Payload is two
  // includes away and resolves; Engine is one away.
  check(has_edge(graph, "consume", "Payload", "references"), "transitive include reference -> Payload");
  check(has_edge(graph, "consume", "Engine", "references"), "direct include reference -> Engine");

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
  // An overloaded name cannot be disambiguated without types, but dropping every
  // call to it would be a regression: before labels became bare names the overload
  // set collapsed onto one node and the call resolved. It now resolves to the first
  // declaration, graded INFERRED.
  check(has_edge(graph, "caller", "overloaded", "CALLS"), "a call to an overloaded name still resolves");

  // A template callee is never tail-reduced, so no phantom call to the struct.
  // `::` appears in nine distinct callee node types, so no text rule can name a
  // callee. `ns::made<zoo::Beast>` reduced at its last `::` yields `Beast>`, which
  // make_id turns into `Beast` -- a fabricated call to an unrelated struct.
  check(!has_edge(graph, "drive", "Beast", "CALLS"),
        "a qualified template callee does not fabricate a call to its type argument");
  check(has_edge(graph, "drive", "wrapper", "CALLS"), "an unqualified template callee resolves");
  check(has_edge(graph, "drive", "made", "CALLS"), "a qualified template callee resolves to its leaf name");
  check(has_edge(graph, "drive", "plain", "CALLS"), "a plain qualified callee resolves");
  check(has_edge(graph, "drive", "only_real_call", "CALLS"), "the real call beside it still resolves");
  // Explicit global scope stays global: it must not bind to a same-named local.
  check(!has_edge(graph, "probe", "stat_local", "CALLS"),
        "a ::global call does not resolve to a same-named local symbol");
  // Three same-line overloads are three nodes, not one.
  {
    int triples = 0;
    for (const auto& node : graph.nodes) {
      triples += (node.kind == "function" && node.label == "triple") ? 1 : 0;
    }
    check(triples == 3, "three overloads sharing one line survive as three nodes");
  }

  // The callee side is keyed on its name too.
  check(has_edge(graph, "callee_user", "free_fn", "CALLS"), "qualified call ns::free_fn resolves");
  check(has_edge(graph, "callee_user", "method", "CALLS"), "member calls obj.f() and ptr->f() resolve");
  check(has_edge(graph, "via_implicit", "method", "CALLS"), "unqualified call to a sibling method resolves");
  // A member call that misses its own file binds to a project-wide unique
  // METHOD (#44)...
  check(has_edge(graph, "reaches_across", "only_over_here", "CALLS"),
        "member call resolves to the project-unique method in another file");
  // ...but never to a free function of the same name, and never to an
  // ambiguous method name.
  check(!has_edge(graph, "reaches_across", "freestanding", "CALLS"),
        "member call does not bind to a free function");
  check(!has_edge(graph, "calls_twin", "twin_method", "CALLS"),
        "member call does not bind to an ambiguous method name");

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
  // Qualified callees: scope must agree with the declaration.
  check(!has_edge(graph, "stdlib_user", "find", "CALLS"), "std::find must not bind to proj::find");
  check(!has_edge(graph, "stdlib_user", "exists", "CALLS"), "std::filesystem::exists must not bind to proj::exists");
  check(has_edge(graph, "stdlib_user", "helper", "CALLS"), "proj::detail::helper resolves through its namespace");
  check(has_edge(graph, "stdlib_user", "size_of", "CALLS"), "proj::Stats::size_of resolves through its class");
  check(has_edge(graph, "stdlib_user", "count_of", "CALLS"), "proj::Stats::count_of resolves through its class");
  // `v.size()` on an unknown receiver must not reach the project's only method named `size`.
  check(!has_edge(graph, "stdlib_user", "size", "CALLS"), "v.size() must not bind to proj::Stats::size");
  check(has_edge(graph, "template_user", "make", "CALLS"), "proj::Outer<int>::make() resolves through the template's class");
  check(has_edge(graph, "hidden_user", "hidden", "CALLS"), "proj::hidden() reaches a symbol in an anonymous namespace");
  {
    // Exactly one `dup` edge, and it is the one declared in namespace alpha.
    int alpha_edges = 0;
    int file_scope_edges = 0;
    for (const auto& edge : graph.edges) {
      if (edge.relation != "CALLS") {
        continue;
      }
      for (const auto& node : graph.nodes) {
        if (node.id != edge.target || node.label != "dup") {
          continue;
        }
        const auto scope = node.properties.find("scope");
        if (scope != node.properties.end() && scope->second == "alpha") {
          ++alpha_edges;
        } else {
          ++file_scope_edges;
        }
      }
    }
    check(alpha_edges == 1 && file_scope_edges == 0, "alpha::dup() edges only to the alpha member of the overload set");
  }
  {
    bool anonymous_scope = false;
    for (const auto& node : graph.nodes) {
      if (node.label == "hidden" && node.kind == "function") {
        const auto scope = node.properties.find("scope");
        anonymous_scope = scope != node.properties.end() && scope->second == "proj::(anonymous)";
      }
    }
    check(anonymous_scope, "an anonymous namespace is spelled (anonymous) in scope");
  }
  {
    bool scoped = false;
    for (const auto& node : graph.nodes) {
      if (node.label == "helper" && node.kind == "function") {
        const auto scope = node.properties.find("scope");
        scoped = scope != node.properties.end() && scope->second == "proj::detail";
      }
    }
    check(scoped, "a symbol declared inside namespaces carries its scope");
  }

  // A qualifier only refuses what it contradicts. The gate used to demand that
  // every candidate PROVE its scope, and an out-of-line definition proved
  // nothing: `int Cache::reload() {}` recorded only the namespace it sat in, a
  // call through an alias or a template parameter nothing that could match.
  check(has_edge(graph, "user_outofline", "reload", "CALLS"),
        "an out-of-line member definition carries the class it qualifies");
  check(has_edge(graph, "user_nested", "spin", "CALLS"),
        "a nested class's out-of-line definition carries both of its segments");
  check(has_edge(graph, "via_alias", "helper_decl", "CALLS"),
        "a namespace alias resolves to the namespace it aliases");
  check(has_edge(graph, "via_inline_ns", "versioned", "CALLS"),
        "an inline namespace is transparent to a qualified call");
  check(has_edge(graph, "build", "make", "CALLS"),
        "a dependent call T::make() names no scope and resolves on its leaf name");
  check(has_edge(graph, "user_inline", "inline_reload", "CALLS"),
        "an in-class definition still resolves through its class");
  // A qualifier rooted in a namespace the project does not declare names a
  // scope no declaration here can be in, whatever the leaf name matches.
  check(!has_edge(graph, "library_user", "format", "CALLS"), "fmt::format does not bind a project format");
  check(!has_edge(graph, "library_user", "trim", "CALLS"), "boost::algorithm::trim does not bind a project trim");
  check(!has_edge(graph, "library_user", "capacity", "CALLS"),
        "absl::strings_internal::capacity does not bind a project capacity");
  check(!has_edge(graph, "library_user", "number", "CALLS"), "QString::number does not bind a project number");
  {
    bool qualified = false;
    for (const auto& node : graph.nodes) {
      if (node.label == "reload" && node.kind == "function") {
        const auto scope = node.properties.find("scope");
        qualified = scope != node.properties.end() && scope->second == "proj::Cache";
      }
    }
    check(qualified, "the class an out-of-line definition names is recorded as its scope");
  }

  return failures == 0 ? 0 : 1;
}
