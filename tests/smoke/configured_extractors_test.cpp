#include "cgraph/configured_extractors.hpp"

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
  if (!check_coverage_registry()) {
    return 3;
  }

  return 0;
}
