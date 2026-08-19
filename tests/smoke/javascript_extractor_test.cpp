#include "cgraph/javascript_extractor.hpp"

#include <string_view>

namespace {

[[nodiscard]] bool has_edge(const cgraph::Fragment& fragment, std::string_view relation, std::string_view target_label) {
  for (const auto& edge : fragment.edges) {
    if (edge.relation != relation) {
      continue;
    }
    for (const auto& node : fragment.nodes) {
      if (node.id == edge.target && node.label == target_label) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

int main() {
  constexpr auto js_source = R"js(
import fs from "fs";

class Worker {
  run() {
    return helper();
  }
}

function helper() {
  return fs.readFileSync("x");
}
)js";

  const auto js_result = cgraph::extract_javascript(
      cgraph::ExtractionContext{.source_file = "worker.js", .source = js_source});

  if (js_result.fragment.nodes.size() < 3) {
    return 1;
  }
  if (js_result.raw_calls.empty()) {
    return 1;
  }
  // `import fs from "fs"` -> a module node reached via imports_from and the
  // default specifier reached via imports.
  if (!has_edge(js_result.fragment, "imports_from", "fs")) {
    return 1;
  }
  if (!has_edge(js_result.fragment, "imports", "fs")) {
    return 1;
  }

  constexpr auto ts_source = R"ts(
import type { Config } from "./config";
export { Helper } from "./helper";

export class Service {
  run(config: Config) {
    return build(config);
  }
}

function build(config: Config) {
  return config.name;
}
)ts";

  const auto ts_result = cgraph::extract_typescript(
      cgraph::ExtractionContext{.source_file = "service.ts", .source = ts_source});

  if (ts_result.fragment.nodes.size() < 3) {
    return 1;
  }
  if (ts_result.raw_calls.empty()) {
    return 1;
  }
  // `import { Config } from "./config"` -> imports_from the module, imports the
  // named symbol.
  if (!has_edge(ts_result.fragment, "imports_from", "./config")) {
    return 1;
  }
  if (!has_edge(ts_result.fragment, "imports", "Config")) {
    return 1;
  }
  // `export { Helper } from "./helper"` -> re_exports the module and the symbol.
  if (!has_edge(ts_result.fragment, "re_exports", "./helper")) {
    return 1;
  }
  if (!has_edge(ts_result.fragment, "re_exports", "Helper")) {
    return 1;
  }
  // A local `export class Service` must NOT create a spurious re_exports module
  // edge — the class is captured by the normal contains walk instead.
  if (has_edge(ts_result.fragment, "re_exports", "Service")) {
    return 1;
  }

  // TS type-level declarations become first-class nodes (kind "type").
  constexpr auto types_source = R"ts(
export interface User { id: string; }
export type Handler = (e: Event) => void;
export enum Color { Red, Green }
)ts";
  const auto types_result = cgraph::extract_typescript(
      cgraph::ExtractionContext{.source_file = "types.ts", .source = types_source});
  std::size_t type_nodes = 0;
  bool saw_user = false;
  bool saw_handler = false;
  bool saw_color = false;
  for (const auto& node : types_result.fragment.nodes) {
    if (node.kind == "type") {
      ++type_nodes;
    }
    saw_user = saw_user || (node.label == "User" && node.kind == "type");
    saw_handler = saw_handler || (node.label == "Handler" && node.kind == "type");
    saw_color = saw_color || (node.label == "Color" && node.kind == "type");
  }
  if (type_nodes != 3 || !saw_user || !saw_handler || !saw_color) {
    return 1;
  }
  // Each is contained by its file.
  if (!has_edge(types_result.fragment, "contains", "User")) {
    return 1;
  }

  // Module-level consts with a factory/object/array value (Zustand stores,
  // contexts, config literals) become first-class "variable" nodes so calls to
  // them resolve. A const defined *inside* a function is a local and must not.
  constexpr auto store_source = R"ts(
import { create } from "zustand";
export const useStore = create(() => ({ count: 0 }));
const config = { url: "/api" };
function Component() {
  const local = makeLocal();
  return local;
}
)ts";
  const auto store_result = cgraph::extract_typescript(
      cgraph::ExtractionContext{.source_file = "store.ts", .source = store_source});
  bool saw_use_store = false;
  bool saw_config = false;
  bool saw_local = false;
  for (const auto& node : store_result.fragment.nodes) {
    saw_use_store = saw_use_store || (node.label == "useStore" && node.kind == "variable");
    saw_config = saw_config || (node.label == "config" && node.kind == "variable");
    saw_local = saw_local || node.label == "local";
  }
  if (!saw_use_store || !saw_config) {
    return 1;  // module-level factory/object consts must be nodes
  }
  if (saw_local) {
    return 1;  // a const inside a function body is a local, not a node
  }
  if (!has_edge(store_result.fragment, "contains", "useStore")) {
    return 1;
  }

  // Heritage and member type references become raw relation facts (resolved to
  // edges after merge). Primitive types (void) are excluded from references.
  constexpr auto rel_source = R"ts(
import { Base } from "./base";
import type { Config } from "./config";
export class Service extends Base implements Handler {
  run(cfg: Config): Result { return cfg as Result; }
  widget: Widget;
}
interface Handler extends Listener {
  onEvent(e: Evt): void;
}
)ts";
  const auto rel_result = cgraph::extract_typescript(
      cgraph::ExtractionContext{.source_file = "service.ts", .source = rel_source});
  const auto has_relation = [&](std::string_view relation, std::string_view target) {
    for (const auto& r : rel_result.raw_relations) {
      if (r.relation == relation && r.target_label == target) {
        return true;
      }
    }
    return false;
  };
  if (!has_relation("inherits", "Base") || !has_relation("implements", "Handler")) {
    return 1;  // class heritage
  }
  if (!has_relation("inherits", "Listener")) {
    return 1;  // interface inheritance
  }
  if (!has_relation("references", "Config") || !has_relation("references", "Result") ||
      !has_relation("references", "Widget") || !has_relation("references", "Evt")) {
    return 1;  // parameter / return / field / interface-method param types
  }
  if (has_relation("references", "void")) {
    return 1;  // primitive types must be filtered
  }

  // Import stub ids must NEVER collide with real node ids. With an extensioned
  // relative import (`from './chunkBy.ts'`, the es-toolkit house style) the
  // resolved module spec IS the imported file's path, and the naive ids
  // make_id(path) / make_id(path + ":" + name) are byte-identical to the real
  // file and symbol node ids -- merge_fragments keeps the first occurrence per
  // id, so an importer that merges first squats the real ids and the imported
  // file vanishes from the graph (650 of 1508 es-toolkit files, issue #39).
  {
    constexpr auto importer_source = R"ts(
import { chunkBy } from './chunkBy.ts';
export function chunkByPipe() {
  return chunkBy([]);
}
)ts";
    const auto importer = cgraph::extract_typescript(
        cgraph::ExtractionContext{.source_file = "src/array/fp.ts", .source = importer_source});
    constexpr auto imported_source = R"ts(
export function chunkBy() {
  return [];
}
)ts";
    const auto imported = cgraph::extract_typescript(
        cgraph::ExtractionContext{.source_file = "src/array/chunkBy.ts", .source = imported_source});
    for (const auto& stub : importer.fragment.nodes) {
      if (stub.kind != "import" && stub.kind != "module") {
        continue;
      }
      for (const auto& real : imported.fragment.nodes) {
        if (stub.id == real.id) {
          return 1;  // a stub id squatting a real node id erases that node at merge
        }
      }
    }
  }

  return 0;
}
