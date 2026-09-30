#include "cgraph/python_extractor.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/graph_builder.hpp"
#include "cgraph/normalize.hpp"
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

struct BuiltPython {
  cgraph::GraphSnapshot graph;
  cgraph::ContractResolution stats;
};

// Extract, merge, resolve imports, resolve contracts: the order run_one_shot uses.
BuiltPython build_python(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<cgraph::Fragment> fragments;
  std::vector<cgraph::RawRelation> relations;
  for (const auto& [path, source] : files) {
    const auto result = cgraph::extract_python({.source_file = "/proj/" + path, .relative_path = path, .source = source});
    fragments.push_back(result.fragment);
    relations.insert(relations.end(), result.raw_relations.begin(), result.raw_relations.end());
  }
  BuiltPython built;
  built.graph = cgraph::merge_fragments(fragments);
  cgraph::resolve_imports(built.graph);
  cgraph::resolve_contracts(built.graph, relations, &built.stats);
  return built;
}

bool has_edge(const cgraph::GraphSnapshot& graph, const std::string& source, const std::string& target,
              const std::string& relation) {
  for (const auto& edge : graph.edges) {
    if (edge.source == source && edge.target == target && edge.relation == relation) return true;
  }
  return false;
}

int fail(const std::string& message) {
  std::cerr << "python_extractor_test: " << message << '\n';
  return 1;
}

// FastAPI routers become HTTP providers. The fixture has ml-backend's shapes
// (api/app.py, api/routes/**, api/legacy/__init__.py): APIRouter(prefix=)
// chains, @router.<verb>(...) handlers (one multi-line, one with an empty
// path), include_router mounts composed across files through aliased imports
// and a package __init__, a prefix on include_router, and an app-level route.
int fastapi_routes() {
  const auto built = build_python({
      {"api/app.py", R"py(
from fastapi import FastAPI
from api.legacy import router as legacy_router
from api.routes.project.router import router as project_router

app = FastAPI(title="ML Backend")
app.include_router(project_router)
app.include_router(legacy_router)


@app.get("/healthcheck")
async def healthcheck():
    return {}


def create_app():
    inner = FastAPI()
    inner.include_router(project_router, prefix="/nested")

    @inner.get("/inside")
    def inside():
        return {}

    return inner
)py"},
      {"api/routes/project/router.py", R"py(
from fastapi import APIRouter

from api.routes.project.cluster.routes import router as cluster_router
from api.routes.project.setup.routes import router as setup_router

router = APIRouter()
router.include_router(setup_router)
router.include_router(cluster_router)
)py"},
      {"api/routes/project/setup/routes.py", R"py(
from fastapi import APIRouter, Depends

router = APIRouter(prefix="/project", tags=["Project API"])


@router.post("/{project_id}/setup", response_model=dict)
async def setup(project_id: str):
    return {}


@router.get(
    "/{project_id}/formulations/score",
    response_model=dict,
)
async def score(project_id: str):
    return {}
)py"},
      {"api/routes/project/cluster/routes.py", R"py(
from fastapi import APIRouter

router = APIRouter(prefix="/simulate/cluster", tags=["Simulate API"])


@router.get("")
async def cluster_status():
    return {}


@router.api_route("/wake", methods=["POST", "PUT"])
async def wake():
    return {}
)py"},
      {"api/legacy/__init__.py", R"py(
from fastapi import APIRouter

from api.legacy.routes import design_router, dynamic_router

router = APIRouter()
router.include_router(design_router, prefix="/v1/model", tags=["legacy"])
router.include_router(dynamic_router, prefix=settings.PREFIX)
)py"},
      {"api/legacy/routes.py", R"py(
from fastapi import APIRouter

design_router = APIRouter()
dynamic_router = APIRouter()
elsewhere = APIRouter(prefix=settings.PREFIX)


@design_router.get("/{model_id}/variables", status_code=200)
async def get_model_variables(model_id: str):
    return {}


@dynamic_router.get("/dynamic")
async def dynamic():
    return {}


@elsewhere.get("/elsewhere")
async def elsewhere_route():
    return {}


@design_router.get(f"/{PREFIX}/computed")
async def computed():
    return {}
)py"},
  });

  struct Expected {
    std::string id;
    std::string handler;
    std::string file;
  };
  const std::vector<Expected> expected = {
      {"endpoint:GET /healthcheck", "api/app.py:healthcheck", "api/app.py"},
      {"endpoint:POST /project/{}/setup", "api/routes/project/setup/routes.py:setup", "api/routes/project/setup/routes.py"},
      {"endpoint:GET /project/{}/formulations/score", "api/routes/project/setup/routes.py:score", "api/routes/project/setup/routes.py"},
      {"endpoint:GET /simulate/cluster", "api/routes/project/cluster/routes.py:cluster_status", "api/routes/project/cluster/routes.py"},
      {"endpoint:POST /simulate/cluster/wake", "api/routes/project/cluster/routes.py:wake", "api/routes/project/cluster/routes.py"},
      {"endpoint:PUT /simulate/cluster/wake", "api/routes/project/cluster/routes.py:wake", "api/routes/project/cluster/routes.py"},
      {"endpoint:GET /v1/model/{}/variables", "api/legacy/routes.py:get_model_variables", "api/legacy/routes.py"},
  };
  for (const auto& want : expected) {
    const cgraph::Node* found = nullptr;
    for (const auto& node : built.graph.nodes) {
      if (node.id == want.id) found = &node;
    }
    if (found == nullptr || found->kind != "endpoint") return fail("missing " + want.id);
    if (found->properties.contains("served")) return fail(want.id + " is not served");
    if (!has_edge(built.graph, want.id, cgraph::make_id(want.handler), "handled_by")) return fail(want.id + " handler");
    if (!has_edge(built.graph, cgraph::make_id(want.file), want.id, "contains")) return fail(want.id + " contains");
  }
  // An import of a router binds to its variable, which its file contains, so
  // the importer still reaches the imported file.
  if (!has_edge(built.graph, cgraph::make_id("api/routes/project/router.py"),
                cgraph::make_id("api/routes/project/setup/routes.py:router"), "imports") ||
      !has_edge(built.graph, cgraph::make_id("api/routes/project/setup/routes.py"),
                cgraph::make_id("api/routes/project/setup/routes.py:router"), "contains")) {
    return fail("router variable import/contains");
  }
  // The provider's spelling survives as the label; the id is canonical.
  for (const auto& node : built.graph.nodes) {
    if (node.id == "endpoint:POST /project/{}/setup" && node.label != "POST /project/{project_id}/setup") {
      return fail("label " + node.label);
    }
  }
  // Exactly these seven. Four routes have no knowable path and mint nothing,
  // each counted: `inside` (its app is a local in a factory), `elsewhere_route`
  // (APIRouter(prefix=settings.PREFIX)), `dynamic` (its only mount's prefix is
  // not a literal, so serving it at `/dynamic` would be wrong) and `computed`
  // (an f-string path). The factory's mount and the non-literal mount count as
  // unresolved mounts.
  std::size_t endpoint_count = 0;
  for (const auto& node : built.graph.nodes) endpoint_count += node.kind == "endpoint" ? 1 : 0;
  if (endpoint_count != expected.size()) return fail("endpoint count " + std::to_string(endpoint_count));
  if (built.stats.routes != 11) return fail("routes " + std::to_string(built.stats.routes));
  if (built.stats.routes_unresolved != 4) return fail("routes_unresolved " + std::to_string(built.stats.routes_unresolved));
  if (built.stats.mounts != 7) return fail("mounts " + std::to_string(built.stats.mounts));
  if (built.stats.mounts_unresolved != 2) return fail("mounts_unresolved " + std::to_string(built.stats.mounts_unresolved));
  return 0;
}

}  // namespace

int main() {
  if (const auto status = fastapi_routes(); status != 0) return status;

  const auto members = cgraph::extract_python({.source_file = "members.py", .relative_path = "members.py", .source = R"py(
from dataclasses import dataclass
@dataclass
class First:
    size: int
    name: str = ""
    def method(self):
        local: int = 0
class Second:
    size: int
    name: str
)py"});
  for (const std::string owner : {"First", "Second"}) {
    std::set<std::string> labels;
    for (const auto& edge : members.fragment.edges) {
      if (edge.relation != "defines" || edge.source != cgraph::make_id("members.py:" + owner)) continue;
      for (const auto& field : members.fragment.nodes) {
        if (field.id != edge.target) continue;
        if (field.kind != "field" || field.id != cgraph::make_id("members.py:" + owner + "::" + field.label)) return 1;
        if (field.properties.at("type_text") != (field.label == "size" ? "int" : "str")) return 1;
        labels.insert(field.label);
      }
    }
    if (labels != std::set<std::string>{"size", "name"}) return 1;
  }

  // make_id collapses `First::size` and `first_size` onto one id
  // (normalize.cpp), and merge_fragments keeps only the first node with an id.
  // The function must keep the plain id an agent queries it by, and the field
  // must still exist and be the target of its owner's `defines` edge.
  const auto collision = cgraph::extract_python({.source_file = "c.py", .relative_path = "c.py", .source = R"py(
def first_size():
    return 1
class First:
    size: int
)py"});
  const cgraph::Node* collided_function = nullptr;
  const cgraph::Node* collided_field = nullptr;
  for (const auto& node : collision.fragment.nodes) {
    if (node.kind == "function" && node.label == "first_size") collided_function = &node;
    if (node.kind == "field" && node.label == "size") collided_field = &node;
  }
  if (collided_function == nullptr || collided_field == nullptr) return 1;
  if (collided_function->id != cgraph::make_id("c.py:first_size")) return 1;
  if (collided_field->id == collided_function->id) return 1;
  bool defines_the_field = false;
  for (const auto& edge : collision.fragment.edges) {
    if (edge.relation != "defines" || edge.source != cgraph::make_id("c.py:First")) continue;
    if (edge.target == collided_function->id) return 1;
    defines_the_field = defines_the_field || edge.target == collided_field->id;
  }
  if (!defines_the_field) return 1;

  constexpr auto source = R"py(
import os
from pathlib import Path

class Worker:
    def run(self):
        return Path.cwd()
)py";

  const auto result = cgraph::extract_python(
      cgraph::ExtractionContext{.source_file = "worker.py", .relative_path = "worker.py", .source = source});

  if (result.fragment.nodes.size() < 4) {
    return 1;
  }
  if (result.raw_calls.empty()) {
    return 1;
  }

  bool saw_import = false;
  for (const auto& node : result.fragment.nodes) {
    if (node.kind == "import") {
      saw_import = true;
    }
  }
  if (!saw_import) {
    return 1;
  }

  // A root-level relative import and an absolute import of the same name get
  // different stub ids (relative vs absolute namespace); import_path stays
  // absolute for the relative one.
  {
    const auto imported = cgraph::extract_python({.source_file = "/abs/proj/app.py", .relative_path = "app.py", .source = R"py(
from .config import Local
from config import Remote
)py"});
    const auto relative_id = cgraph::make_id("import-relative-symbol:config:Local");
    const auto absolute_id = cgraph::make_id("import-symbol:config:Remote");
    std::string relative_path_prop;
    bool absolute_module = false;
    for (const auto& node : imported.fragment.nodes) {
      const auto path = node.properties.contains("import_path") ? node.properties.at("import_path") : std::string{};
      if (node.id == relative_id) relative_path_prop = path;
      if (node.id == absolute_id) absolute_module = path == "config";
    }
    if (relative_path_prop != "/abs/proj/config" || !absolute_module) return 1;
    if (cgraph::make_id("import-relative-module:config") == cgraph::make_id("import-module:config")) return 1;
  }

  return 0;
}
