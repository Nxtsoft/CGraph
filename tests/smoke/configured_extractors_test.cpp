#include "cgraph/configured_extractors.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

const cgraph::Node* find_node(const cgraph::ExtractionResult& result, std::string_view label, std::string_view kind) {
  for (const auto& node : result.fragment.nodes) {
    if (node.label == label && node.kind == kind) {
      return &node;
    }
  }
  return nullptr;
}

bool fail(std::string_view message) {
  std::cerr << message << '\n';
  return false;
}

// The C# config drives the generic walker: namespace/class declarations,
// methods, a plain call, and a member call (obj.Method) recorded same-file so
// resolution never guesses across files.
bool check_csharp_extraction() {
  constexpr std::string_view source =
      "using System;\n"
      "using App.Services;\n"
      "\n"
      "namespace Demo\n"
      "{\n"
      "    public class Service\n"
      "    {\n"
      "        public void Run()\n"
      "        {\n"
      "            Helper();\n"
      "            Console.WriteLine(\"up\");\n"
      "        }\n"
      "        private void Helper() { }\n"
      "    }\n"
      "}\n";

  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::CSharp,
      cgraph::ExtractionContext{.source_file = "Demo/Service.cs", .source = source});
  if (!result.has_value()) {
    return fail("csharp extraction returned no result");
  }
  if (find_node(*result, "Service", "class") == nullptr) {
    return fail("missing class node Service");
  }
  if (find_node(*result, "Run", "function") == nullptr) {
    return fail("missing method node Run");
  }
  if (find_node(*result, "Helper", "function") == nullptr) {
    return fail("missing method node Helper");
  }
  const auto plain_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "Helper" && !call.is_member_call;
  });
  if (plain_call == result->raw_calls.end()) {
    return fail("missing plain raw call to Helper");
  }
  // Console.WriteLine: a member_access call carries the bare member name, flagged
  // member so resolution never guesses the receiver/type across files.
  const auto member_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "WriteLine" && call.is_member_call;
  });
  if (member_call == result->raw_calls.end()) {
    return fail("missing member raw call to WriteLine");
  }
  return true;
}

// The Go config drives the generic walker end to end: named types, functions,
// pointer-receiver methods, quoted imports (module stub + imports edge), plain
// calls, and selector calls recorded as same-file member calls.
bool check_go_extraction() {
  constexpr std::string_view source =
      "package main\n"
      "\n"
      "import (\n"
      "\t\"fmt\"\n"
      "\t\"example.com/app/internal/auth\"\n"
      ")\n"
      "\n"
      "type Service struct{}\n"
      "\n"
      "type Handler interface {\n"
      "\tHandle() error\n"
      "}\n"
      "\n"
      "type ID = int64\n"
      "\n"
      "func (s *Service) Run() {\n"
      "\thelper()\n"
      "\tfmt.Println(\"up\")\n"
      "}\n"
      "\n"
      "func helper() {}\n";

  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Go,
      cgraph::ExtractionContext{.source_file = "app/service.go", .source = source});
  if (!result.has_value()) {
    return fail("go extraction returned no result");
  }

  if (find_node(*result, "Service", "type") == nullptr) {
    return fail("missing type node Service");
  }
  if (find_node(*result, "Handler", "type") == nullptr) {
    return fail("missing type node Handler");
  }
  if (find_node(*result, "ID", "type") == nullptr) {
    return fail("missing type-alias node ID");
  }
  if (find_node(*result, "Run", "function") == nullptr) {
    return fail("missing method node Run");
  }
  if (find_node(*result, "helper", "function") == nullptr) {
    return fail("missing function node helper");
  }
  if (find_node(*result, "fmt", "module") == nullptr ||
      find_node(*result, "example.com/app/internal/auth", "module") == nullptr) {
    return fail("missing import module stubs");
  }

  const bool has_import_edge = std::ranges::any_of(result->fragment.edges, [](const cgraph::Edge& edge) {
    return edge.relation == "imports";
  });
  if (!has_import_edge) {
    return fail("missing imports edge");
  }

  const auto plain_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "helper" && !call.is_member_call;
  });
  if (plain_call == result->raw_calls.end()) {
    return fail("missing plain raw call to helper");
  }
  // fmt.Println: a selector call carries the bare field name, flagged member so
  // resolution never guesses across files.
  const auto member_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "Println" && call.is_member_call;
  });
  if (member_call == result->raw_calls.end()) {
    return fail("missing member raw call to Println");
  }
  return true;
}

bool check_rust_extraction() {
  constexpr std::string_view source =
      "struct Service {}\n"
      "enum Status { Ok, Err }\n"
      "trait Handler { fn handle(&self); }\n"
      "type Id = i64;\n"
      "\n"
      "impl Service {\n"
      "    fn run(&self) {\n"
      "        helper();\n"
      "        self.tick();\n"
      "        Service::make();\n"
      "        assert_eq!(mhelper(), 1);\n"
      "        assert!(self.mtick());\n"
      "    }\n"
      "    fn tick(&self) {}\n"
      "    fn mtick(&self) -> bool { true }\n"
      "    fn make() -> Service { Service {} }\n"
      "}\n"
      "\n"
      "fn helper() {}\n"
      "fn mhelper() -> i32 { 1 }\n";

  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Rust,
      cgraph::ExtractionContext{.source_file = "src/service.rs", .source = source});
  if (!result.has_value()) {
    return fail("rust extraction returned no result");
  }

  // struct/enum/trait/type-alias are all `type` nodes (no class kind in Rust).
  for (const char* type_name : {"Service", "Status", "Handler", "Id"}) {
    if (find_node(*result, type_name, "type") == nullptr) {
      return fail(std::string("missing type node ") + type_name);
    }
  }
  // impl methods are function_items and are captured as functions; `handle` is a
  // trait function_signature_item (no body); `helper` is a free function.
  for (const char* fn_name : {"run", "tick", "make", "helper", "handle"}) {
    if (find_node(*result, fn_name, "function") == nullptr) {
      return fail(std::string("missing function node ") + fn_name);
    }
  }

  // helper() -> a plain identifier call, non-member.
  const auto plain_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "helper" && !call.is_member_call;
  });
  if (plain_call == result->raw_calls.end()) {
    return fail("missing plain raw call to helper");
  }
  // self.tick() -> call_expression{function: field_expression}; the bare field
  // name is a member call so resolution stays same-file.
  const auto member_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "tick" && call.is_member_call;
  });
  if (member_call == result->raw_calls.end()) {
    return fail("missing member raw call to tick");
  }
  // Service::make() -> a scoped_identifier callee. cpp_callee_name would drop it;
  // rust_callee_name reduces it to the bare name `make`, kept as a non-member
  // call eligible for project-wide resolution.
  const auto scoped_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "make" && !call.is_member_call;
  });
  if (scoped_call == result->raw_calls.end()) {
    return fail("missing scoped raw call reduced to make");
  }

  // --- issue #58 ---
  // assert_eq!(mhelper(), 1): a call inside a macro invocation is token-tree
  // content, not a call_expression; the macro scan must still record it.
  const auto macro_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "mhelper" && !call.is_member_call;
  });
  if (macro_call == result->raw_calls.end()) {
    return fail("missing macro-wrapped raw call to mhelper");
  }
  // assert!(self.mtick()): `. ident (` inside a macro is a member call.
  const auto macro_member = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "mtick" && call.is_member_call;
  });
  if (macro_member == result->raw_calls.end()) {
    return fail("missing macro-wrapped member raw call to mtick");
  }
  // The macro names themselves are not calls.
  if (std::ranges::any_of(result->raw_calls, [](const cgraph::RawCall& call) {
        return call.callee_label == "assert_eq" || call.callee_label == "assert";
      })) {
    return fail("macro name recorded as a call");
  }
  // An impl method is tagged so member-call resolution can see it; a free
  // function and a trait signature are not. Contract nodes (interface_method)
  // are separate namespaced nodes and are excluded from the check.
  const auto method_tag = [&](const char* fn_name) {
    for (const auto& node : result->fragment.nodes) {
      if (node.label != fn_name || node.kind != "function" ||
          node.properties.contains("interface_method")) {
        continue;
      }
      const auto tag = node.properties.find("method");
      return tag != node.properties.end() && tag->second == "true";
    }
    return false;
  };
  for (const char* fn_name : {"run", "tick", "make"}) {
    if (!method_tag(fn_name)) {
      return fail(std::string("impl method not tagged: ") + fn_name);
    }
  }
  for (const char* fn_name : {"helper", "handle"}) {
    if (method_tag(fn_name)) {
      return fail(std::string("non-impl function wrongly tagged: ") + fn_name);
    }
  }
  // Each impl method binds to its self type via a method_of fact.
  const auto method_of = std::ranges::count_if(result->raw_relations, [](const cgraph::RawRelation& rel) {
    return rel.relation == "method_of" && rel.target_label == "Service";
  });
  if (method_of != 4) {  // run, tick, mtick, make
    return fail("expected 4 method_of facts binding impl methods to Service, got " +
                std::to_string(method_of));
  }
  // The trait's promised method is materialized as a contract node (tagged
  // interface_method) owned by the trait via a `method` edge — the mirror of
  // Go's interface handling, feeding implements/dispatches_to resolution.
  const auto contract = std::ranges::find_if(result->fragment.nodes, [](const cgraph::Node& node) {
    const auto tag = node.properties.find("interface_method");
    return node.label == "handle" && tag != node.properties.end() && tag->second == "true";
  });
  if (contract == result->fragment.nodes.end()) {
    return fail("missing trait contract node for handle");
  }
  const auto contract_owned = std::ranges::any_of(result->fragment.edges, [&](const cgraph::Edge& edge) {
    return edge.relation == "method" && edge.target == contract->id;
  });
  if (!contract_owned) {
    return fail("trait contract node not owned via a method edge");
  }
  return true;
}

// Rust `use` declarations: one stub per imported leaf. The path syntax alone
// cannot distinguish a module from an item declared in a module, so extraction
// records the full `/`-joined path plus a `module_layout=rust` marker and defers
// the layout decision (`<path>.rs` / `<path>/mod.rs`, parent-module retry for
// items) to resolve_imports. Aliases resolve via the original name; glob and
// `{self}` leaves name the module itself.
bool check_rust_use_imports() {
  constexpr std::string_view source =
      "use crate::foo::bar::Baz;\n"
      "use crate::util::helper as h;\n"
      "use crate::a::{b::C, d};\n"
      "use crate::e::prelude::*;\n"
      "use serde::Serialize;\n"
      "use crate::m::{self, n};\n"
      "use super::sibling::Thing;\n"
      "\n"
      "fn main() {}\n";

  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Rust,
      cgraph::ExtractionContext{.source_file = "src/main.rs", .source = source});
  if (!result.has_value()) {
    return fail("rust extraction returned no result");
  }

  const auto find_stub = [&](std::string_view label, std::string_view kind,
                             std::string_view import_path) -> const cgraph::Node* {
    for (const auto& node : result->fragment.nodes) {
      if (node.label != label || node.kind != kind) {
        continue;
      }
      const auto path = node.properties.find("import_path");
      if (path == node.properties.end() || path->second != import_path) {
        continue;
      }
      const auto layout = node.properties.find("module_layout");
      if (layout == node.properties.end() || layout->second != "rust") {
        continue;
      }
      return &node;
    }
    return nullptr;
  };

  struct Expected {
    const char* label;
    const char* kind;
    const char* path;
  };
  const Expected expected[] = {
      {"Baz", "import", "foo/bar/Baz"},         // plain scoped item, crate:: stripped
      {"helper", "import", "util/helper"},      // alias: original name, never `h`
      {"C", "import", "a/b/C"},                 // nested scoped leaf inside a list
      {"d", "import", "a/d"},                   // plain leaf inside a list
      {"e/prelude", "module", "e/prelude"},     // glob names the module itself
      {"Serialize", "import", "serde/Serialize"},  // external crate: still a stub here
      {"m", "module", "m"},                     // {self} names the module itself
      {"n", "import", "m/n"},                   // sibling leaf of the self import
      {"Thing", "import", "sibling/Thing"},     // super:: stripped
  };
  for (const auto& item : expected) {
    if (find_stub(item.label, item.kind, item.path) == nullptr) {
      return fail(std::string("missing rust use stub ") + item.label + " (" + item.path + ")");
    }
  }
  // The alias must not surface as its own node.
  for (const auto& node : result->fragment.nodes) {
    if (node.label == "h") {
      return fail("alias `h` leaked into the fragment");
    }
  }

  const auto file_id = cgraph::make_id("src/main.rs");
  std::size_t import_edges = 0;
  for (const auto& edge : result->fragment.edges) {
    if (edge.relation == "imports" && edge.source == file_id) {
      ++import_edges;
    }
  }
  if (import_edges < std::size(expected)) {
    return fail("missing file -> stub imports edges");
  }
  return true;
}

bool check_coverage_registry() {
  if (!cgraph::has_registered_extractor(cgraph::DetectedLanguage::Go) ||
      !cgraph::has_registered_extractor(cgraph::DetectedLanguage::Python) ||
      !cgraph::has_registered_extractor(cgraph::DetectedLanguage::Sql) ||
      !cgraph::has_registered_extractor(cgraph::DetectedLanguage::CSharp)) {
    return fail("has_registered_extractor false for a supported language");
  }
  if (cgraph::has_registered_extractor(cgraph::DetectedLanguage::PhpBlade) ||
      cgraph::has_registered_extractor(cgraph::DetectedLanguage::Unknown)) {
    return fail("has_registered_extractor true for an unsupported language");
  }

  const std::vector<cgraph::DetectedFile> files = {
      {.path = "a.go", .language = cgraph::DetectedLanguage::Go},
      {.path = "b.cs", .language = cgraph::DetectedLanguage::CSharp},
      {.path = "view.blade.php", .language = cgraph::DetectedLanguage::PhpBlade},
      {.path = "layout.blade.php", .language = cgraph::DetectedLanguage::PhpBlade},
      {.path = "junk", .language = cgraph::DetectedLanguage::Unknown},
  };
  const auto counts = cgraph::unextracted_counts(files);
  // C# is now extracted, so it must not appear in the unextracted tally; the two
  // Blade files (still unsupported) are the only ones counted.
  if (counts.size() != 1 || counts.at("php-blade") != 2 || counts.contains("csharp")) {
    return fail("unextracted_counts mismatch");
  }
  return true;
}

// fwcd/tree-sitter-kotlin exposes no named fields on its declarations or its
// call_expression, so without the positional resolvers (kotlin_symbol_name /
// kotlin_callee_name) a Kotlin file extracts zero symbols and zero calls. This
// drives the whole generic walker: a class, an object declaration, an interface
// (spelled class_declaration in this grammar), named functions, a plain call,
// and a `recv.member()` navigation call reduced to its bare name and — mirroring
// Java's method_invocation — kept non-member so it resolves project-wide across
// files.
bool check_kotlin_extraction() {
  constexpr std::string_view source =
      "package com.example\n"
      "\n"
      "interface Shape {\n"
      "    fun area(): Double\n"
      "}\n"
      "\n"
      "object Registry {\n"
      "    fun lookup(): Int = 0\n"
      "}\n"
      "\n"
      "class Service : Shape {\n"
      "    override fun area(): Double = 1.0\n"
      "    fun run() {\n"
      "        helper()\n"
      "        Registry.lookup()\n"
      "    }\n"
      "    fun helper() {}\n"
      "}\n";

  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Kotlin,
      cgraph::ExtractionContext{.source_file = "com/example/Service.kt", .source = source});
  if (!result.has_value()) {
    return fail("kotlin extraction returned no result");
  }
  // interface (class_declaration), object_declaration, and class are all "class"
  // nodes named by their type_identifier child.
  for (const char* class_name : {"Shape", "Registry", "Service"}) {
    if (find_node(*result, class_name, "class") == nullptr) {
      return fail(std::string("missing class node ") + class_name);
    }
  }
  // Functions are named by their simple_identifier child (not its return type).
  for (const char* fn_name : {"area", "lookup", "run", "helper"}) {
    if (find_node(*result, fn_name, "function") == nullptr) {
      return fail(std::string("missing function node ") + fn_name);
    }
  }
  // helper() -> a bare simple_identifier callee, non-member.
  const auto plain_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "helper" && !call.is_member_call;
  });
  if (plain_call == result->raw_calls.end()) {
    return fail("missing plain raw call to helper");
  }
  // Registry.lookup() -> a navigation_expression callee reduced to the bare member
  // name `lookup`, kept non-member (like Java) so it resolves project-wide. Before
  // the resolver the callee would have been the whole `Registry.lookup(...)` text.
  const auto nav_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "lookup" && !call.is_member_call;
  });
  if (nav_call == result->raw_calls.end()) {
    return fail("missing navigation raw call reduced to lookup");
  }
  return true;
}

}  // namespace

int main() {
  const auto languages = {
      cgraph::DetectedLanguage::C,
      cgraph::DetectedLanguage::Cpp,
      cgraph::DetectedLanguage::CSharp,
      cgraph::DetectedLanguage::Go,
      cgraph::DetectedLanguage::Groovy,
      cgraph::DetectedLanguage::Java,
      cgraph::DetectedLanguage::JavaScript,
      cgraph::DetectedLanguage::Kotlin,
      cgraph::DetectedLanguage::Python,
      cgraph::DetectedLanguage::Ruby,
      cgraph::DetectedLanguage::Rust,
      cgraph::DetectedLanguage::Scala,
      cgraph::DetectedLanguage::TypeScript,
      cgraph::DetectedLanguage::Tsx,
  };

  for (const auto language : languages) {
    auto config = cgraph::config_for_language(language);
    if (!config.has_value()) {
      return 1;
    }
    if (config->name.empty() || config->grammar_name.empty() || config->extensions.empty()) {
      return 1;
    }
    if (config->function_node_types.empty() && config->class_node_types.empty()) {
      return 1;
    }
  }

  if (cgraph::config_for_language(cgraph::DetectedLanguage::McpConfig).has_value()) {
    return 1;
  }

  if (!check_go_extraction()) {
    return 2;
  }
  if (!check_csharp_extraction()) {
    return 4;
  }
  if (!check_rust_extraction()) {
    return 5;
  }
  if (!check_rust_use_imports()) {
    return 6;
  }
  if (!check_kotlin_extraction()) {
    return 7;
  }
  if (!check_coverage_registry()) {
    return 3;
  }

  return 0;
}
