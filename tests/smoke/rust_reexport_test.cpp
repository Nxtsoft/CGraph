// Cross-crate `pub use` re-export following (issue #60). A consumer that spells
// `producer::thing::Widget` reaches a module (thing/mod.rs) that only re-exports
// the item -- `pub use inner::Widget` -- rather than defining it. Item resolution
// lands on the module file and, without following the re-export, stops there:
// the reverse walk from the real (and changed) definition in inner.rs never
// reaches the consumer. This is tokio-util's tests reaching a changed
// `tokio::task::LocalSet` through `pub use local::LocalSet` in task/mod.rs. The
// resolver now follows the `pub use` chain to the defining file.
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
  const auto root = std::filesystem::temp_directory_path() / "cgraph_rust_reexport_test";
  std::filesystem::remove_all(root);

  // producer crate: Widget is defined in thing/inner.rs and only re-exported by
  // thing/mod.rs.
  write_file(root / "producer" / "src" / "lib.rs", "pub mod thing;\n");
  write_file(root / "producer" / "src" / "thing" / "mod.rs",
             "pub mod inner;\n\npub use inner::Widget;\n");
  write_file(root / "producer" / "src" / "thing" / "inner.rs",
             "pub struct Widget;\n\n"
             "impl Widget {\n"
             "    pub fn go(&self) -> u8 { 1 }\n"
             "}\n");
  // consumer crate: an integration test spells the item by the producer's crate
  // name, through the re-exporting module path.
  write_file(root / "consumer" / "tests" / "use_it.rs",
             "use producer::thing::Widget;\n\n"
             "#[test]\n"
             "fn exercises_widget() {\n"
             "    let w = Widget;\n"
             "    let _ = w.go();\n"
             "}\n");

  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;
  std::filesystem::remove_all(root);

  const auto file_id = [&](std::string_view suffix) -> std::string {
    for (const auto& node : graph.nodes) {
      if (node.kind == "file" && node.source_file.ends_with(suffix)) {
        return node.id;
      }
    }
    return {};
  };
  const auto symbol_in = [&](std::string_view label, std::string_view file_suffix) -> std::string {
    for (const auto& node : graph.nodes) {
      if (node.label == label && node.kind != "file" && node.source_file.ends_with(file_suffix)) {
        return node.id;
      }
    }
    return {};
  };
  const auto has_edge = [&](const std::string& source, const std::string& target) {
    return !source.empty() && !target.empty() &&
           std::ranges::any_of(graph.edges, [&](const auto& e) {
             return e.source == source && e.target == target;
           });
  };

  const auto consumer = file_id("use_it.rs");
  const auto mod_file = file_id("thing/mod.rs");
  const auto widget_def = symbol_in("Widget", "inner.rs");  // the real definition

  if (consumer.empty() || widget_def.empty()) {
    return 1;
  }
  // The payload: the consumer's import reaches the defining file's symbol, not a
  // dead-end at the re-exporting module. Without fix (c), no edge from the
  // consumer reaches inner.rs's Widget at all.
  if (!has_edge(consumer, widget_def)) {
    return 2;
  }
  // The re-export edge itself resolved from the module to the real definition.
  if (!mod_file.empty() && !has_edge(mod_file, widget_def)) {
    return 3;
  }
  return 0;
}
