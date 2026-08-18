#include "cgraph/configured_extractors.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

bool has_label(const cgraph::ExtractionResult& result, std::string_view label_fragment) {
  for (const auto& node : result.fragment.nodes) {
    if (node.label.find(label_fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool check(
    cgraph::DetectedLanguage language,
    std::string_view file,
    std::string_view source,
    std::string_view expected_label) {
  auto result = cgraph::extract_configured_language(
      language,
      cgraph::ExtractionContext{
          .source_file = std::string(file),
          .source = source,
      });
  if (!result.has_value()) {
    std::cerr << "no extraction result for " << file << '\n';
    return false;
  }
  if (!has_label(*result, expected_label)) {
    std::cerr << "missing label fragment '" << expected_label << "' for " << file << "; saw:";
    for (const auto& node : result->fragment.nodes) {
      std::cerr << " [" << node.kind << ":" << node.label << "]";
    }
    std::cerr << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  struct Case {
    cgraph::DetectedLanguage language;
    std::string_view file;
    std::string_view source;
    std::string_view expected_label;
  };

  const std::vector<Case> cases = {
      {cgraph::DetectedLanguage::C, "main.c", "int main(void) { return 0; }", "main"},
      {cgraph::DetectedLanguage::Cpp, "service.cpp", "class Service {}; int run() { return 0; }", "Service"},
      {cgraph::DetectedLanguage::Java, "Service.java", "class Service { void run() {} }", "Service"},
      {cgraph::DetectedLanguage::JavaScript, "service.js", "class Service { run() { return helper(); } }", "Service"},
      {cgraph::DetectedLanguage::TypeScript, "service.ts", "export class Service { run(): void {} }", "Service"},
      {cgraph::DetectedLanguage::Python, "service.py", "class Service:\n    def run(self):\n        pass\n", "Service"},
      {cgraph::DetectedLanguage::Ruby, "service.rb", "class Service\n  def run\n  end\nend\n", "Service"},
      {cgraph::DetectedLanguage::Kotlin, "Service.kt", "class Service { fun run() {} }", "Service"},
      {cgraph::DetectedLanguage::Scala, "Service.scala", "class Service { def run(): Unit = {} }", "Service"},
      {cgraph::DetectedLanguage::Groovy, "Service.groovy", "class Service { def run() {} }", "Service"},
      {cgraph::DetectedLanguage::Go, "service.go",
       "package main\n\ntype Service struct{}\n\nfunc (s *Service) Run() {}\n", "Service"},
      {cgraph::DetectedLanguage::Rust, "service.rs",
       "struct Service {}\nimpl Service { fn run(&self) { helper(); } }\nfn helper() {}\n", "Service"},
      // A `use` leaf becomes an import stub carrying the original item name
      // (consumed later by resolve_imports; see configured_extractors_test for
      // the full use-tree shapes).
      {cgraph::DetectedLanguage::Rust, "imports.rs",
       "use crate::foo::bar::Baz;\nfn main() {}\n", "Baz"},
      {cgraph::DetectedLanguage::MsBuild, "app.csproj", R"xml(<Project><Target Name="Build"/></Project>)xml", "Build"},
      {cgraph::DetectedLanguage::Delphi, "Form1.dfm", "object Form1: TForm1\nend\n", "Form1"},
      {cgraph::DetectedLanguage::Apex, "Worker.cls", "public class Worker { public void run() {} }", "Worker"},
      {cgraph::DetectedLanguage::McpConfig, "mcp.json", R"json({"mcpServers":{"filesystem":{"command":"npx"}}})json", "filesystem"},
  };

  for (const auto& test_case : cases) {
    if (!check(test_case.language, test_case.file, test_case.source, test_case.expected_label)) {
      return 1;
    }
  }

  return 0;
}
