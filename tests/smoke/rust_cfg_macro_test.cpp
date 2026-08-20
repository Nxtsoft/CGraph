// Macro-interior item extraction for Rust (issue #63). tokio gates most of its
// definitions behind `cfg_*!` item-wrapper macros (`cfg_rt!`, `cfg_coop!`,
// `cfg_io_util!`, ... — 317 sites in tokio/src); tree-sitter leaves a macro body
// an opaque token_tree, so the fn/impl/struct inside is never extracted. That is
// why `SimplexStream::poll_read` — wrapped in `cfg_coop!` and the only caller of
// the changed `poll_read_internal` — was invisible, leaving the reverse walk
// dead. The extractor now blanks `cfg_*! { ... }` wrappers before parsing, so the
// items inside parse in place: as real impl methods, at their real line numbers,
// with their calls resolved.
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
  const auto root = std::filesystem::temp_directory_path() / "cgraph_rust_cfg_macro_test";
  std::filesystem::remove_all(root);

  // `poll_read` is wrapped in cfg_coop! (line 9); it calls the inherent
  // `poll_read_internal` (line 4). Without unwrapping, poll_read is not a node
  // and poll_read_internal has no caller.
  write_file(root / "lib.rs",
             "pub struct Stream {\n"          // 1
             "    buf: u8,\n"                  // 2
             "}\n"                             // 3
             "impl Stream {\n"                 // 4
             "    fn poll_read_internal(&self) -> u8 {\n"  // 5
             "        self.buf\n"              // 6
             "    }\n"                         // 7
             "}\n"                             // 8
             "impl Stream {\n"                 // 9
             "    cfg_coop! {\n"               // 10
             "        fn poll_read(&self) -> u8 {\n"       // 11
             "            self.poll_read_internal()\n"     // 12
             "        }\n"                     // 13
             "    }\n"                         // 14
             "}\n");                           // 15

  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;
  std::filesystem::remove_all(root);

  const auto node_at = [&](std::string_view label) -> const cgraph::Node* {
    for (const auto& node : graph.nodes) {
      if (node.label == label) return &node;
    }
    return nullptr;
  };
  const auto edge = [&](std::string_view relation, const std::string& source, const std::string& target) {
    return !source.empty() && !target.empty() &&
           std::ranges::any_of(graph.edges, [&](const auto& e) {
             return e.relation == relation && e.source == source && e.target == target;
           });
  };

  const auto* poll_read = node_at("poll_read");
  const auto* internal = node_at("poll_read_internal");

  // The macro-wrapped method surfaced as a node at all.
  if (poll_read == nullptr || internal == nullptr) {
    return 1;
  }
  // Its enclosing impl context is preserved: it is a method, not a free fn.
  const auto method = poll_read->properties.find("method");
  if (method == poll_read->properties.end() || method->second != "true") {
    return 2;
  }
  // Its line number is real (11), not shifted by the rewrite.
  if (!poll_read->source_location.has_value() || poll_read->source_location->start_line != 11) {
    return 3;
  }
  // Its call resolves: the changed inherent method now has a caller, so a
  // reverse walk from poll_read_internal reaches poll_read.
  if (!edge("CALLS", poll_read->id, internal->id)) {
    return 4;
  }
  return 0;
}
