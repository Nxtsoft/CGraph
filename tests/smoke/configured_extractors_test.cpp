#include "cgraph/configured_extractors.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/normalize.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>
#include <set>
#include <string>
#include <utility>

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
      cgraph::ExtractionContext{.source_file = "Demo/Service.cs", .relative_path = "Demo/Service.cs", .source = source});
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
      cgraph::ExtractionContext{.source_file = "app/service.go", .relative_path = "app/service.go", .source = source});
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
      cgraph::ExtractionContext{.source_file = "src/service.rs", .relative_path = "src/service.rs", .source = source});
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
      cgraph::ExtractionContext{.source_file = "src/main.rs", .relative_path = "src/main.rs", .source = source});
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

// Java constructor-call resolution: a `new Foo()` callee is an
// object_creation_expression whose `type` field (not a `name` identifier) must be
// reduced to the bare class name so the call resolves against the class node.
// java_callee_name handles the plain, generic, and package-qualified forms.
bool check_java_extraction() {
  constexpr std::string_view source =
      "package p;\n"
      "import java.util.ArrayList;\n"
      "public class Widget {\n"
      "  public void build() {\n"
      "    Widget w = new Widget();\n"
      "    ArrayList<String> xs = new ArrayList<String>();\n"
      "    java.util.HashMap<String, Integer> m = new java.util.HashMap<String, Integer>();\n"
      "    helper();\n"
      "  }\n"
      "  void helper() {}\n"
      "}\n";

  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Java,
      cgraph::ExtractionContext{.source_file = "p/Widget.java", .relative_path = "p/Widget.java", .source = source});
  if (!result.has_value()) {
    return fail("java extraction returned no result");
  }
  if (find_node(*result, "Widget", "class") == nullptr) {
    return fail("missing class node Widget");
  }
  for (const char* fn_name : {"build", "helper"}) {
    if (find_node(*result, fn_name, "function") == nullptr) {
      return fail(std::string("missing method node ") + fn_name);
    }
  }
  // Each constructor callee is reduced to the bare simple type name and kept
  // non-member (a plain `new Foo()` resolves project-wide to the class node).
  for (const char* type_name : {"Widget", "ArrayList", "HashMap"}) {
    const auto ctor = std::ranges::find_if(result->raw_calls, [&](const cgraph::RawCall& call) {
      return call.callee_label == type_name && !call.is_member_call;
    });
    if (ctor == result->raw_calls.end()) {
      return fail(std::string("missing constructor raw call reduced to ") + type_name);
    }
  }
  // A plain method_invocation still resolves by its `name` field, unchanged.
  const auto plain_call = std::ranges::find_if(result->raw_calls, [](const cgraph::RawCall& call) {
    return call.callee_label == "helper" && !call.is_member_call;
  });
  if (plain_call == result->raw_calls.end()) {
    return fail("missing plain raw call to helper");
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
      cgraph::ExtractionContext{.source_file = "com/example/Service.kt", .relative_path = "com/example/Service.kt", .source = source});
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

// HTTP client calls in Kotlin (Ktor) and Go (net/http and client wrappers)
// record the same `http_call` / `http_wrapper` facts the JavaScript extractor
// does: "<relation> <target_label> <context>" per fact, source checked apart.
std::multiset<std::string> http_facts(const cgraph::ExtractionResult& result) {
  std::multiset<std::string> facts;
  for (const auto& relation : result.raw_relations) {
    if (relation.relation == "http_call" || relation.relation == "http_wrapper") {
      facts.insert(relation.relation + " " + relation.target_label + " " + relation.context);
    }
  }
  return facts;
}

bool same_facts(const std::multiset<std::string>& got, const std::multiset<std::string>& want, std::string_view what) {
  if (got == want) return true;
  for (const auto& fact : got) std::cerr << "  got:  " << fact << '\n';
  for (const auto& fact : want) std::cerr << "  want: " << fact << '\n';
  return fail(what);
}

// Extracts one file, then merges and resolves contracts as a build does.
struct ResolvedFile {
  cgraph::GraphSnapshot graph;
  cgraph::ContractResolution stats;
};

std::optional<ResolvedFile> resolve_file(cgraph::DetectedLanguage language, const std::string& path,
                                         std::string_view source) {
  const auto result =
      cgraph::extract_configured_language(language, {.source_file = path, .relative_path = path, .source = source});
  if (!result) return std::nullopt;
  const std::vector<cgraph::Fragment> fragments{result->fragment};
  ResolvedFile resolved{.graph = cgraph::merge_fragments(fragments), .stats = {}};
  cgraph::resolve_imports(resolved.graph);
  cgraph::resolve_contracts(resolved.graph, result->raw_relations, &resolved.stats);
  return resolved;
}

std::vector<std::string> consumes_edges(const cgraph::GraphSnapshot& graph) {
  std::vector<std::string> edges;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "CONSUMES") edges.push_back(edge.source + " -> " + edge.target);
  }
  return edges;
}

// Receivers that merely contain "client" or "http" are no HTTP client.
constexpr std::string_view kKotlinNotClients = R"kt(
class Repo(private val clientRepository: ClientRepository, private val clients: Map<String, Client>, private val httpCache: Cache) {
    fun a(e: Entity) = clientRepository.delete(e)
    fun b(id: String) = clients.get(id)
    fun c() = httpCache.get("/api/v1/cached")
    fun d() = clientRepository.get("/api/v1/looks-like-a-path")
    fun find(key: String) = clients.get(key)
    fun lookupDefault() = find("/api/v1/not-an-endpoint")
    fun real() = ktorClient.get("$baseUrl/api/v1/real")
}
)kt";

constexpr std::string_view kKotlinClient = R"kt(
class SessionsApi(private val baseUrl: String, private val client: HttpClient) {
    suspend fun sessions(token: String) = client.get("$baseUrl/api/v1/sessions/user/me") { bearerAuth(token) }
    suspend fun revoke(sessionId: String) {
        val response = client.patch("$baseUrl/api/v1/sessions/$sessionId/invalidate") { bearerAuth(t) }
    }
    suspend fun remove(deviceId: String) = request(send = { token -> client.delete("${baseUrl}/api/v1/mfa/devices/${deviceId}?force=true") })
    suspend fun login(id: String) = postOutcome(path = "/api/v1/auth/login", request = id)
    private suspend fun postOutcome(path: String, request: Any) = client.post("$baseUrl$path") { setBody(request) }
    suspend fun approve(id: String) = decide("token", "$baseUrl/api/v1/proposals/$id/approve")
    private suspend fun decide(token: String, url: String) = httpClient.post(url) { bearerAuth(token) }
    suspend fun probe(p: Probe) = client.request("$baseUrl${p.endpoint}") { method = HttpMethod.parse(p.method) }
    suspend fun stacked() = client.get("$host$prefix/users")
    suspend fun external() = client.get("https://example.com/api/v1/x")
    fun notRequests(cache: Map<String, String>) { cache.get("/api/v1/key"); restClient.post().uri("/api/v1/x"); log("GET", "fine") }
}
)kt";

bool check_kotlin_http_clients() {
  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Kotlin,
      {.source_file = "SessionsApi.kt", .relative_path = "SessionsApi.kt", .source = kKotlinClient});
  if (!result) return fail("kotlin client extraction failed");
  if (!same_facts(http_facts(*result),
                  {"http_call client.get GET /api/v1/sessions/user/me",
                   "http_call client.patch PATCH /api/v1/sessions/{}/invalidate",
                   "http_call client.delete DELETE /api/v1/mfa/devices/{}",
                   "http_call postOutcome  /api/v1/auth/login",
                   "http_wrapper client.post POST ",
                   "http_call decide  /api/v1/proposals/{}/approve",
                   "http_wrapper httpClient.post POST ",
                   // Unresolvable: counted by resolve_contracts, never guessed.
                   "http_call client.request  ",
                   "http_call client.get GET ",
                   "http_call client.get GET "},
                  "kotlin http facts")) {
    return false;
  }
  for (const auto& relation : result->raw_relations) {
    if (relation.relation == "http_wrapper" && relation.target_label == "client.post" &&
        relation.source_id != cgraph::make_id("SessionsApi.kt:postOutcome")) {
      return fail("kotlin wrapper is the function whose parameter is the URL tail: " + relation.source_id);
    }
    if (relation.relation == "http_call" && relation.target_label == "client.delete" &&
        relation.source_id != cgraph::make_id("SessionsApi.kt:remove")) {
      return fail("a request inside a lambda belongs to the enclosing function: " + relation.source_id);
    }
  }
  const auto negatives = resolve_file(cgraph::DetectedLanguage::Kotlin, "Repo.kt", kKotlinNotClients);
  if (!negatives) return fail("kotlin extraction failed");
  const auto edges = consumes_edges(negatives->graph);
  const std::vector<std::string> want{cgraph::make_id("Repo.kt:real") + " -> endpoint:GET /api/v1/real"};
  if (negatives->stats.calls != 1 || negatives->stats.calls_unresolved != 0 || edges != want) {
    std::cerr << "calls=" << negatives->stats.calls << " unresolved=" << negatives->stats.calls_unresolved << '\n';
    for (const auto& edge : edges) std::cerr << "  consumes: " << edge << '\n';
    return fail("only a receiver named as an http client is a kotlin request");
  }
  return true;
}

// Calls that pass an HTTP verb and a path but send no request: route
// registrations (a handler value, no context), test helpers building
// server-side requests, assertions on a request's method, logging, and a
// non-HTTP subcommand. None may count as a client call.
constexpr std::string_view kGoNotClients = R"go(package server

func Register(r *gin.Engine, c chi.Router, e *echo.Echo, router *httprouter.Router) {
	r.Handle(http.MethodGet, "/api/v1/users", listUsers)
	c.Method(http.MethodPost, "/api/v1/orders", createOrder)
	e.Add("DELETE", "/api/v1/items/:id", deleteItem)
	router.HandlerFunc("PUT", "/api/v1/widgets", putWidget)
	srv.Route(ctx, http.MethodGet, "/api/v1/hooks", func(w http.ResponseWriter, r *http.Request) {})
}

func TestHandler(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/api/v1/users", nil)
	req = httptest.NewRequestWithContext(ctx, http.MethodGet, "/api/v1/users", nil)
	httpmock.RegisterResponder("GET", "/api/v1/mocked", httpmock.NewStringResponder(200, "{}"))
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		assert.Equal(t, http.MethodPost, r.Method)
		require.Equal(t, "GET", r.Method)
	}))
}

func Other(ctx context.Context) {
	log.Println("GET", "/api/v1/logged")
	out, err := git.Run(ctx, "rev-parse", "--abbrev-ref", "HEAD", "--quiet")
	out, err = git.Run(ctx, "HEAD", "--quiet")
}

func Real(cmd *cobra.Command) {
	s.Client.Mutate(cmd.Context(), "PATCH", "/api/v1/things/"+id+"/enable", nil)
}
)go";

constexpr std::string_view kGoClient = R"go(package api

func (c *Client) Login(ctx context.Context, req LoginRequest) (*LoginResponse, error) {
	return c.postAuth(ctx, "/api/v1/auth/login", req)
}

func (c *Client) postAuth(ctx context.Context, path string, req any) (*LoginResponse, error) {
	resp, err := c.Do(ctx, http.MethodPost, path, body)
	return nil, fmt.Errorf("decoding response from %s: %w", path, err)
}

func (c *Client) Me(ctx context.Context) (*UserInfo, error) {
	resp, err := c.Do(ctx, http.MethodGet, "/api/v1/auth/me", nil)
	_, err = scoped.DoForm(ctx, "POST", `/oauth2/token`, form)
	_, err = c.Do(ctx, http.MethodDelete, "/api/v1/users/"+id+"/sessions", nil)
	_, err = s.Client.Mutate(ctx, "POST", n.Base+"/import", body)
	return nil, err
}

func (c *Client) ListPage(ctx context.Context, path string) (*Page, error) {
	resp, err := c.Do(ctx, http.MethodGet, path+"?"+q.Encode(), nil)
	return nil, err
}

func (c *Client) once(ctx context.Context, method, path string) {
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+ensureLeadingSlash(path), rdr)
	resp, err := http.Get(base + "/healthz")
	if strings.HasPrefix(path, "/") {
	}
}
)go";

bool check_go_http_clients() {
  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Go,
      {.source_file = "internal/api/auth.go", .relative_path = "internal/api/auth.go", .source = kGoClient});
  if (!result) return fail("go client extraction failed");
  if (!same_facts(http_facts(*result),
                  {"http_call postAuth  /api/v1/auth/login",
                   "http_wrapper c.Do POST ",
                   "http_call c.Do GET /api/v1/auth/me",
                   "http_call scoped.DoForm POST /oauth2/token",
                   "http_call c.Do DELETE /api/v1/users/{}/sessions",
                   // `path+"?"+...` is not a prefix wrapper; a method held in a
                   // variable and a URL built by a call are unresolvable.
                   "http_call c.Do GET ",
                   "http_call Client.Mutate POST ",  // `n.Base` is a path prefix, not a host
                   "http_call http.NewRequestWithContext  ",
                   "http_call http.Get GET /healthz"},
                  "go http facts")) {
    return false;
  }
  for (const auto& relation : result->raw_relations) {
    if (relation.relation == "http_wrapper" && relation.source_id != cgraph::make_id("internal/api/auth.go:postAuth")) {
      return fail("go wrapper is postAuth: " + relation.source_id);
    }
  }
  const auto negatives = resolve_file(cgraph::DetectedLanguage::Go, "server.go", kGoNotClients);
  if (!negatives) return fail("go extraction failed");
  const auto edges = consumes_edges(negatives->graph);
  const std::vector<std::string> want{cgraph::make_id("server.go:Real") + " -> endpoint:PATCH /api/v1/things/{}/enable"};
  if (negatives->stats.calls != 1 || negatives->stats.calls_unresolved != 0 || edges != want) {
    std::cerr << "calls=" << negatives->stats.calls << " unresolved=" << negatives->stats.calls_unresolved << '\n';
    for (const auto& edge : edges) std::cerr << "  consumes: " << edge << '\n';
    return fail("only the context-carrying client call with a URL is a go request");
  }
  return true;
}

// End to end: a Spring controller serves the routes, and the Kotlin and Go
// clients' calls (direct and through their wrappers) consume the same endpoint
// ids, so a workspace links them.
bool check_http_clients_consume_spring_routes() {
  const std::vector<std::pair<cgraph::DetectedLanguage, std::pair<std::string, std::string>>> files = {
      {cgraph::DetectedLanguage::Kotlin, {"AuthController.kt", R"kt(
@RestController
@RequestMapping("/api/v1/auth")
class AuthController {
    @PostMapping("/login")
    fun login(@RequestBody body: LoginRequest) = service.login(body)
}
@RestController
@RequestMapping("/api/v1/sessions")
class SessionController {
    @PatchMapping("/{id}/invalidate")
    fun invalidateSession(@PathVariable id: String) = service.invalidate(id)
}
)kt"}},
      {cgraph::DetectedLanguage::Kotlin, {"SessionsApi.kt", std::string(kKotlinClient)}},
      {cgraph::DetectedLanguage::Go, {"internal/api/auth.go", std::string(kGoClient)}},
  };
  std::vector<cgraph::Fragment> fragments;
  std::vector<cgraph::RawRelation> relations;
  for (const auto& [language, file] : files) {
    const auto result = cgraph::extract_configured_language(
        language, {.source_file = file.first, .relative_path = file.first, .source = file.second});
    if (!result) return fail("extraction failed for " + file.first);
    fragments.push_back(result->fragment);
    relations.insert(relations.end(), result->raw_relations.begin(), result->raw_relations.end());
  }
  auto graph = cgraph::merge_fragments(fragments);
  cgraph::resolve_imports(graph);
  cgraph::ContractResolution stats;
  cgraph::resolve_contracts(graph, relations, &stats);
  const auto has_edge = [&](const std::string& source, const std::string& target, std::string_view relation) {
    return std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
      return edge.source == source && edge.target == target && edge.relation == relation;
    });
  };
  const std::string login = "endpoint:POST /api/v1/auth/login";
  const std::string invalidate = "endpoint:PATCH /api/v1/sessions/{}/invalidate";
  if (!has_edge(login, cgraph::make_id("AuthController.kt:login"), "handled_by") ||
      !has_edge(invalidate, cgraph::make_id("AuthController.kt:invalidateSession"), "handled_by")) {
    return fail("spring endpoints are handled by their annotated methods");
  }
  const std::pair<std::string, std::string> consumers[] = {
      {cgraph::make_id("SessionsApi.kt:login"), login},              // Kotlin wrapper call
      {cgraph::make_id("SessionsApi.kt:revoke"), invalidate},        // Kotlin `$sessionId` segment
      {cgraph::make_id("internal/api/auth.go:Login"), login},        // Go wrapper call
      {cgraph::make_id("internal/api/auth.go:Me"), "endpoint:GET /api/v1/auth/me"},
  };
  for (const auto& [consumer, target] : consumers) {
    if (!has_edge(consumer, target, "CONSUMES")) return fail(consumer + " should consume " + target);
  }
  // Unresolvable Kotlin/Go calls are tallied, never guessed.
  // Kotlin: client.request, "$host$prefix", an absolute URL; Go: a query tail,
  // a base-relative path behind a value, and NewRequestWithContext with a
  // variable verb and a call-built URL.
  if (stats.calls_unresolved != 6) {
    std::cerr << "calls=" << stats.calls << " unresolved=" << stats.calls_unresolved << '\n';
    return fail("unresolvable client calls are counted");
  }
  return true;
}

// A wrapper whose request runs inside a lambda / function literal still appends
// the enclosing function's parameter; the lambda's own parameters do not count.
bool check_wrappers_through_lambdas() {
  const auto kotlin = resolve_file(cgraph::DetectedLanguage::Kotlin, "Api.kt", R"kt(
class AuthApi(private val baseUrl: String, private val client: HttpClient) {
    private suspend fun postOutcome(path: String, body: Any) = withContext(Dispatchers.IO) { client.post("$baseUrl$path") { setBody(body) } }
    suspend fun login(body: Any) = postOutcome("/api/v1/auth/login", body)
    private suspend fun each(ids: List<String>) = ids.map { path -> client.get("$baseUrl$path") }
    suspend fun all() = each("/api/v1/not-a-tail")
}
)kt");
  if (!kotlin) return fail("kotlin extraction failed");
  const std::vector<std::string> kotlin_want{cgraph::make_id("Api.kt:login") + " -> endpoint:POST /api/v1/auth/login"};
  bool ok = true;
  if (consumes_edges(kotlin->graph) != kotlin_want) {
    for (const auto& edge : consumes_edges(kotlin->graph)) std::cerr << "  consumes: " << edge << '\n';
    ok = fail("a kotlin wrapper's request inside a lambda appends the function's parameter");
  }
  const auto go = resolve_file(cgraph::DetectedLanguage::Go, "c.go", R"go(package c

func (c *Client) post(ctx context.Context, path string, body any) error {
	return retry(func() error {
		_, err := c.Do(ctx, http.MethodPost, path, body)
		return err
	})
}

func (c *Client) Login(ctx context.Context) error {
	return c.post(ctx, "/api/v1/auth/login", nil)
}

func (c *Client) each(ctx context.Context, paths []string) {
	forEach(paths, func(path string) { c.Do(ctx, http.MethodGet, path, nil) })
}

func (c *Client) All(ctx context.Context) {
	c.each(ctx, "/api/v1/not-a-tail")
}
)go");
  if (!go) return fail("go extraction failed");
  const std::vector<std::string> go_want{cgraph::make_id("c.go:Login") + " -> endpoint:POST /api/v1/auth/login"};
  if (consumes_edges(go->graph) != go_want) {
    for (const auto& edge : consumes_edges(go->graph)) std::cerr << "  consumes: " << edge << '\n';
    ok = fail("a go wrapper's request inside a func literal appends the function's parameter");
  }
  return ok;
}

}  // namespace

// Spring request mappings emit `file_route` facts: lowercase verb, class prefix
// joined with the method path, source = the annotated method. Nothing for a
// constant path or a method-less method-level @RequestMapping.
bool check_spring_route_facts() {
  const auto result = cgraph::extract_configured_language(
      cgraph::DetectedLanguage::Kotlin,
      {.source_file = "Users.kt", .relative_path = "Users.kt", .source = R"kt(
@RestController
@RequestMapping("/api/v1/users")
class Users {
    @GetMapping("/{id}")
    fun get(id: String) = id
    @PatchMapping
    fun patch() {}
    @GetMapping(Paths.X)
    fun constant() {}
    @RequestMapping("/any")
    fun any() {}
}
)kt"});
  if (!result) return fail("kotlin extraction failed");
  std::multiset<std::string> facts;
  for (const auto& relation : result->raw_relations) {
    if (relation.relation == "file_route") facts.insert(relation.context);
  }
  if (facts != std::multiset<std::string>{"get /api/v1/users/{id}", "patch /api/v1/users"}) {
    for (const auto& fact : facts) std::cerr << "  fact: " << fact << '\n';
    return fail("spring file_route facts");
  }
  return true;
}

int main() {
  struct MemberCase { cgraph::DetectedLanguage language; std::string source; };
  for (const auto& test : std::vector<MemberCase>{
      {cgraph::DetectedLanguage::Go, "package main\ntype First struct { Size, Count int; Name string }\ntype Second struct { Size, Count int; Name string }"},
      {cgraph::DetectedLanguage::Rust, "struct First { Size: i32, Count: i32, Name: String } struct Second { Size: i32, Count: i32, Name: String }"},
      {cgraph::DetectedLanguage::Java, "class First { int Size, Count; String Name; } record Second(int Size, int Count, String Name) {}"},
  }) {
    const auto result = cgraph::extract_configured_language(test.language, {.source_file = "members", .relative_path = "members", .source = test.source});
    if (!result) return 1;
    for (const std::string owner : {"First", "Second"}) {
      std::set<std::string> labels;
      for (const auto& edge : result->fragment.edges) {
        if (edge.relation != "defines" || edge.source != cgraph::make_id("members:" + owner)) continue;
        for (const auto& field : result->fragment.nodes) {
          if (field.id != edge.target) continue;
          if (field.kind != "field" || field.id != cgraph::make_id("members:" + owner + "::" + field.label)) return 1;
          if (!field.properties.contains("type_text")) return 1;
          labels.insert(field.label);
        }
      }
      if (labels != std::set<std::string>{"Size", "Count", "Name"}) { std::cerr << "missing members on " << owner << '\n'; return 1; }
    }
  }

  // A field must never take the id a function wants: `Config::path` and
  // `config_path` normalize to the same id, and before the shared guard the
  // field claimed it and the function was renamed to `..._2_0` -- a stable
  // function id changing because a struct one line up has a matching member.
  {
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Rust,
        {.source_file = "c.rs",
         .relative_path = "c.rs",
         .source = "pub struct Config { pub path: String }\npub fn config_path() -> u8 { 0 }\n"});
    if (!result) return 1;
    const cgraph::Node* function = nullptr;
    const cgraph::Node* field = nullptr;
    for (const auto& node : result->fragment.nodes) {
      if (node.kind == "function" && node.label == "config_path") function = &node;
      if (node.kind == "field" && node.label == "path") field = &node;
    }
    if (function == nullptr || field == nullptr) return 1;
    if (function->id != cgraph::make_id("c.rs:config_path")) {
      std::cerr << "rust: function id moved to " << function->id << '\n';
      return 1;
    }
    if (field->id == function->id) return 1;
    bool defines_the_field = false;
    for (const auto& edge : result->fragment.edges) {
      if (edge.relation != "defines" || edge.source != cgraph::make_id("c.rs:Config")) continue;
      if (edge.target == function->id) return 1;
      defines_the_field = defines_the_field || edge.target == field->id;
    }
    if (!defines_the_field) return 1;
  }

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
  if (!check_java_extraction()) {
    return 8;
  }
  if (!check_coverage_registry()) {
    return 3;
  }
  if (!check_spring_route_facts()) {
    return 4;
  }
  if (!check_kotlin_http_clients()) {
    return 9;
  }
  if (!check_go_http_clients()) {
    return 10;
  }
  if (!check_http_clients_consume_spring_routes()) {
    return 11;
  }
  if (!check_wrappers_through_lambdas()) {
    return 12;
  }

  return 0;
}
