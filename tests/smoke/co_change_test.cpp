#include "cgraph/co_change.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/pipeline.hpp"
#include "cgraph/protocol.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// The listed files as root-relative paths, in order.
std::vector<std::string> listed(const nlohmann::json& result, const fs::path& root) {
  std::vector<std::string> out;
  for (const auto& row : result.at("files")) {
    out.push_back(fs::path(row.at("source_file").get<std::string>()).lexically_relative(root).generic_string());
  }
  return out;
}

std::string why_of(const nlohmann::json& result, const fs::path& root, std::string_view path) {
  for (const auto& row : result.at("files")) {
    if (fs::path(row.at("source_file").get<std::string>()).lexically_relative(root).generic_string() == path) {
      return row.at("why").get<std::string>();
    }
  }
  return {};
}

int fail(std::string_view what, const nlohmann::json& got) {
  std::cerr << "co_change_test: " << what << ": " << got.dump(2) << '\n';
  return 1;
}

}  // namespace

int main() {
  // The graph records canonical source paths (on macOS the temp dir /var/... is
  // /private/var/...), so the paths it lists are relative to the canonical root.
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / "cgraph_co_change_test";
  fs::remove_all(root);
  // metrics.py is the file being edited. routes.py imports and calls record()
  // (a direct caller), service.py calls routes.handle (a caller of a caller),
  // unrelated.py uses only other(), test_metrics.py is metrics.py's test by
  // name, and lonely.py touches none of it.
  write(root / "pkg" / "metrics.py",
        "def record(value):\n    return value + 1\n\n\ndef other():\n    return 2\n\n\n"
        "class Meter:\n    def tick(self):\n        return record(1)\n");
  write(root / "pkg" / "routes.py", "from pkg.metrics import record\n\n\ndef handle(x):\n    return record(x)\n");
  write(root / "pkg" / "service.py", "from pkg.routes import handle\n\n\ndef serve():\n    return handle(3)\n");
  write(root / "pkg" / "unrelated.py", "from pkg.metrics import other\n\n\ndef use():\n    return other()\n");
  write(root / "pkg" / "test_metrics.py", "def test_nothing():\n    assert True\n");
  write(root / "pkg" / "lonely.py", "def alone():\n    return 0\n");
  const auto graph = cgraph::run_one_shot(root).graph;
  const auto ask = [&](nlohmann::json params) { return cgraph::co_change(graph, params); };

  // Editing record() (lines 1-2): its caller first, then its test by name, then
  // the other files joined to metrics.py; the caller of the caller comes after.
  {
    const auto result = ask({{"id", "pkg_metrics_py"}, {"lines", {{1, 2}}}});
    const auto files = listed(result, root);
    if (!result.at("found").get<bool>() || result.at("edited") != nlohmann::json::array({"record"})) {
      return fail("edited symbols of lines 1-2", result);
    }
    const std::vector<std::string> expected{"pkg/routes.py", "pkg/test_metrics.py", "pkg/unrelated.py", "pkg/service.py"};
    if (files != expected) return fail("ranked files for an edit of record()", result);
    // A call says more than the import of the same symbol.
    if (why_of(result, root, "pkg/routes.py") != "calls `record`") return fail("caller reason", result);
    if (why_of(result, root, "pkg/test_metrics.py") != "test for this file") return fail("test reason", result);
    if (why_of(result, root, "pkg/service.py") != "calls `handle`, which depends on the edited code") {
      return fail("second-hop reason", result);
    }
    if (why_of(result, root, "pkg/unrelated.py") != "calls `other`") return fail("linked reason", result);
    if (result.at("candidates").get<int>() != 4) return fail("candidate count", result);
  }

  // Editing inside a method: the method is the edited symbol, not the class
  // around it, and record() is what the method calls, so no file is a caller.
  {
    const auto result = ask({{"id", "pkg_metrics_py"}, {"lines", {{11, 11}}}});
    if (result.at("edited") != nlohmann::json::array({"tick"})) return fail("innermost edited symbol", result);
    if (listed(result, root).front() != "pkg/test_metrics.py") return fail("no callers: the test comes first", result);
  }

  // Editing other(): unrelated.py is now the direct caller.
  {
    const auto result = ask({{"id", "pkg_metrics_py"}, {"lines", {{5, 6}}}});
    if (listed(result, root).front() != "pkg/unrelated.py" || why_of(result, root, "pkg/unrelated.py") != "calls `other`") {
      return fail("caller of other()", result);
    }
  }

  // A whole-file write (no lines) changes everything the file holds, and the
  // limit caps the list.
  {
    const auto result = ask({{"id", "pkg_metrics_py"}, {"limit", 2}});
    const auto files = listed(result, root);
    if (files != std::vector<std::string>{"pkg/routes.py", "pkg/test_metrics.py"} || result.at("edited").size() != 4) {
      return fail("whole-file write with limit 2", result);
    }
  }

  // The test file names the code it covers; a file joined to nothing names nothing.
  {
    const auto test = ask({{"id", "pkg_test_metrics_py"}, {"lines", {{1, 2}}}});
    if (listed(test, root) != std::vector<std::string>{"pkg/metrics.py"} ||
        why_of(test, root, "pkg/metrics.py") != "the code this test covers") {
      return fail("test to source", test);
    }
    const auto lonely = ask({{"id", "pkg_lonely_py"}, {"lines", {{1, 2}}}});
    if (!lonely.at("found").get<bool>() || !lonely.at("files").empty()) return fail("unconnected file", lonely);
  }

  // An id that is no file node is reported as not found.
  if (const auto missing = ask({{"id", "pkg_metrics_py_record"}}); missing.at("found").get<bool>()) {
    return fail("a symbol id is not a file", missing);
  }

  // Through the daemon: the op answers, and needs an id.
  {
    cgraph::DaemonState state;
    cgraph::publish_graph_snapshot(state, graph);
    const auto response = cgraph::handle_daemon_request(
        state, cgraph::make_request("co-change", {{"id", "pkg_metrics_py"}, {"lines", {{1, 2}}}}));
    if (!response.value("ok", false) || listed(response.at("result"), root).front() != "pkg/routes.py") {
      return fail("daemon co-change", response);
    }
    const auto missing = cgraph::handle_daemon_request(state, cgraph::make_request("co-change", nlohmann::json::object()));
    if (missing.value("ok", true)) return fail("daemon co-change without id", missing);
  }

  fs::remove_all(root);
  std::cout << "co_change_test passed\n";
  return 0;
}
