// Trait-scoped dispatch for Rust (issue #60). A concrete `impl Trait for Type`
// that overrides only the trait's required method — leaving its provided/default
// methods inherited — is invisible to the name-subset dispatch rule: the trait
// promises {poll_read, read} but the type's own method set is only {poll_read},
// so {poll_read, read} is not a subset and no dispatches_to edge forms. This is
// the shape behind tokio's async ext-trait plumbing (`impl AsyncRead for
// SimplexStream` overrides `poll_read`; `read` and the rest stay provided). The
// extractor now emits an `impl_trait` fact from the declared `trait` field, and
// dispatch resolution binds the method to its contract regardless of the subset
// check — so the reverse walk from a changed `poll_read` reaches the contract.
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
  const auto root = std::filesystem::temp_directory_path() / "cgraph_rust_dispatch_test";
  std::filesystem::remove_all(root);

  // `read` is a provided method the impl does NOT override: the trait's method
  // set {poll_read, read} is a strict superset of Widget's {poll_read}, so the
  // subset rule alone yields nothing here.
  write_file(root / "reader.rs",
             "pub trait Reader {\n"
             "    fn poll_read(&self) -> u8;\n"
             "    fn read(&self) -> u8 { self.poll_read() }\n"
             "}\n\n"
             "pub struct Widget;\n\n"
             "impl Reader for Widget {\n"
             "    fn poll_read(&self) -> u8 { 7 }\n"
             "}\n");

  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;
  std::filesystem::remove_all(root);

  const auto iface_method = [&](std::string_view label) -> std::string {
    for (const auto& node : graph.nodes) {
      const auto tag = node.properties.find("interface_method");
      const bool tagged = tag != node.properties.end() && tag->second == "true";
      if (tagged && node.label == label) {
        return node.id;
      }
    }
    return {};
  };
  const auto concrete = [&](std::string_view label) -> std::string {
    for (const auto& node : graph.nodes) {
      if (node.label == label && !node.properties.contains("interface_method")) {
        return node.id;
      }
    }
    return {};
  };
  const auto edge = [&](std::string_view relation, const std::string& source, const std::string& target) {
    return !source.empty() && !target.empty() &&
           std::ranges::any_of(graph.edges, [&](const auto& e) {
             return e.relation == relation && e.source == source && e.target == target;
           });
  };

  const auto reader_iface = concrete("Reader");          // the trait type node
  const auto widget = concrete("Widget");                // the struct type node
  const auto contract_poll_read = iface_method("poll_read");  // trait-method contract
  const auto widget_poll_read = concrete("poll_read");   // the impl method

  // The trait's contract materialized and is owned by the trait node.
  if (contract_poll_read.empty() || !edge("method", reader_iface, contract_poll_read)) {
    return 1;
  }
  // The impl method exists and binds to its self type.
  if (widget_poll_read.empty() || !edge("method_of", widget_poll_read, widget)) {
    return 2;
  }
  // The extractor emitted the declared-trait fact, resolved to the trait node.
  if (!edge("impl_trait", widget_poll_read, reader_iface)) {
    return 3;
  }
  // The payload: dispatch reaches the impl from the contract even though the
  // subset check fails (Widget does not provide `read`). Without fix (a) this
  // edge does not exist.
  if (!edge("dispatches_to", contract_poll_read, widget_poll_read)) {
    return 4;
  }
  // implements still lands, so a reverse walk sees Widget satisfy Reader.
  if (!edge("implements", widget, reader_iface)) {
    return 5;
  }
  return 0;
}
