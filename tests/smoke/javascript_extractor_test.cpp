#include "cgraph/javascript_extractor.hpp"

#include <string_view>
#include "cgraph/normalize.hpp"
#include <set>
#include <string>
#include <vector>

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
  const auto members = cgraph::extract_typescript({.source_file = "members.ts", .source = R"ts(
interface First { readonly id: string; value?: number; nested: { hidden: boolean }; }
interface Second { readonly id: string; value?: number; nested: { hidden: boolean }; }
type Alias = { readonly id: string; value?: number };
enum Choice { One, Two = "two" }
)ts"});
  for (const std::string owner : {"First", "Second", "Alias", "Choice"}) {
    std::set<std::string> labels;
    for (const auto& edge : members.fragment.edges) {
      if (edge.source != cgraph::make_id("members.ts:" + owner) || edge.relation != "defines") continue;
      for (const auto& field : members.fragment.nodes) {
        if (field.id != edge.target) continue;
        if (field.kind != "field" || field.id != cgraph::make_id("members.ts:" + owner + "::" + field.label)) return 1;
        labels.insert(field.label);
        if (field.label == "id" && (field.properties.at("type_text") != "string" || field.properties.at("readonly") != "true")) return 1;
        if (field.label == "value" && (field.properties.at("type_text") != "number" || field.properties.at("optional") != "true")) return 1;
      }
    }
    const std::set<std::string> expected = owner == "Choice" ? std::set<std::string>{"One", "Two"} :
        owner == "Alias" ? std::set<std::string>{"id", "value"} : std::set<std::string>{"id", "value", "nested"};
    if (labels != expected) return 1;
  }

  // TypeScript declaration merging: two owners with one name in one file. Each
  // owner's members must be its own nodes, not one shared node that both owners
  // point a `defines` edge at.
  const auto merged = cgraph::extract_typescript({.source_file = "c.ts", .source = R"ts(
interface Window { locale: string; }
interface Window { locale: string; theme: string; }
)ts"});
  std::vector<std::string> window_owners;
  for (const auto& node : merged.fragment.nodes) {
    if (node.kind == "type" && node.label == "Window") window_owners.push_back(node.id);
  }
  if (window_owners.size() != 2 || window_owners[0] == window_owners[1]) return 1;
  std::set<std::string> locale_targets;
  for (const auto& owner : window_owners) {
    std::size_t locales = 0;
    for (const auto& edge : merged.fragment.edges) {
      if (edge.relation != "defines" || edge.source != owner) continue;
      for (const auto& field : merged.fragment.nodes) {
        if (field.id != edge.target || field.label != "locale") continue;
        ++locales;
        locale_targets.insert(field.id);
      }
    }
    if (locales != 1) return 1;
  }
  if (locale_targets.size() != 2) return 1;

  // `constructor(public readonly x: number)` declares a member; a parameter
  // with no accessibility modifier declares nothing.
  const auto parameters = cgraph::extract_typescript({.source_file = "p.ts", .source = R"ts(
class Point {
  constructor(public readonly x: number, private y?: string, plain: boolean = true) {}
}
)ts"});
  std::set<std::string> declared;
  for (const auto& edge : parameters.fragment.edges) {
    if (edge.relation != "defines" || edge.source != cgraph::make_id("p.ts:Point")) continue;
    for (const auto& field : parameters.fragment.nodes) {
      if (field.id != edge.target || field.kind != "field") continue;
      declared.insert(field.label);
      if (field.label == "x" && (field.properties.at("readonly") != "true" ||
                                 field.properties.at("type_text") != "number")) return 1;
      if (field.label == "y" && (field.properties.at("optional") != "true" ||
                                 field.properties.at("type_text") != "string")) return 1;
    }
  }
  if (declared != std::set<std::string>{"x", "y"}) return 1;

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

  // HTTP route handlers (CGR-4 follow-up). An Elysia module used to be one
  // `variable` node spanning every route; each inline handler is now a named
  // function node and a call scope, so a source anchor lands on the handler and
  // the calls inside it are attributed instead of dropped at the arrow boundary.
  {
    const auto routes = cgraph::extract_typescript({.source_file = "notebooks/index.ts", .source = R"ts(
import { Elysia } from 'elysia';
const notebookRoutes = new Elysia({ prefix: '/notebooks' })
  .use(authWithDbUser)
  .get('/', async ({ dbUser }) => {
    return listNotebooks(dbUser);
  }, { detail: { summary: 'List' } })
  .post('/:id/notes', async ({ dbUser, body }) => {
    return createNote(dbUser, body);
  });
app.get('/health', (req, res) => res.send(ping()));
app.use('/static', (req, res, next) => next());
router.route('/x').get((req, res) => res.end());
const items = list.map(x => transform(x));
describe('suite', () => { run(); });
)ts"});
    const auto find_node = [&](std::string_view label) -> const cgraph::Node* {
      for (const auto& node : routes.fragment.nodes) {
        if (node.label == label) return &node;
      }
      return nullptr;
    };
    // The chain is rooted in a constructor, so the assigned variable names it.
    const auto* list_route = find_node("notebookRoutes.get /");
    const auto* create_route = find_node("notebookRoutes.post /:id/notes");
    // An existing identifier roots the chain directly.
    const auto* health_route = find_node("app.get /health");
    if (list_route == nullptr || create_route == nullptr || health_route == nullptr) return 1;
    for (const auto* handler : {list_route, create_route, health_route}) {
      if (handler->kind != "function" || !handler->source_location) return 1;
    }
    // The handler's extent is the arrow, not the chain: `.get('/', ...)` opens on
    // line 5 and its handler closes on line 7, while the module spans 3-10.
    if (list_route->source_location->start_line != 5 || list_route->source_location->end_line != 7) return 1;
    if (list_route->id != cgraph::make_id("notebooks/index.ts:notebookRoutes.get /")) return 1;
    // The module `variable` node is unchanged: Graphify parity for module consts.
    const auto* module_node = find_node("notebookRoutes");
    if (module_node == nullptr || module_node->kind != "variable") return 1;
    // Each handler is contained by the file and is the caller of its body's calls.
    if (!has_edge(routes.fragment, "contains", "notebookRoutes.get /")) return 1;
    std::set<std::string> callers_of;
    for (const auto& call : routes.raw_calls) {
      if (call.callee_label == "listNotebooks" || call.callee_label == "createNote" || call.callee_label == "ping") {
        callers_of.insert(call.caller_id + "->" + call.callee_label);
      }
    }
    if (callers_of != std::set<std::string>{list_route->id + "->listNotebooks",
                                            create_route->id + "->createNote",
                                            health_route->id + "->ping"}) return 1;
    // Not a route registration: `use` is not a verb, `.route('/x').get(handler)`
    // has no path argument, and `.map` / `describe` callbacks stay boundaries.
    for (const auto& node : routes.fragment.nodes) {
      if (node.kind != "function") continue;
      if (node.label.rfind("app.use", 0) == 0 || node.label.rfind("router", 0) == 0 ||
          node.label.find("map") != std::string::npos || node.label.find("describe") != std::string::npos) return 1;
    }
    for (const auto& call : routes.raw_calls) {
      if (call.callee_label == "transform" || call.callee_label == "run" || call.callee_label == "next") return 1;
    }
  }

  // Express-style middleware: only the last function argument is the handler;
  // middleware before it stays anonymous. A template-literal path is a path.
  {
    const auto express = cgraph::extract_javascript({.source_file = "server.js", .source = R"js(
app.post(`/users`, authenticate, (req, res) => { save(req.body); });
)js"});
    std::size_t handlers = 0;
    for (const auto& node : express.fragment.nodes) {
      if (node.kind != "function") continue;
      if (node.label != "app.post /users") return 1;
      ++handlers;
    }
    if (handlers != 1 || express.raw_calls.size() != 1 || express.raw_calls.front().callee_label != "save" ||
        express.raw_calls.front().caller_id != cgraph::make_id("server.js:app.post /users")) return 1;
  }

  return 0;
}
