#include "cgraph/client_runtime.hpp"

#include "cgraph/daemon_server.hpp"
#include "cgraph/protocol.hpp"
#include "cgraph/workspace.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::optional<nlohmann::json> ok_response(const char* source) {
  return nlohmann::json{{"ok", true}, {"source", source}};
}

bool set_environment(const char* name, const char* value) {
#ifdef _WIN32
  return ::_putenv_s(name, value) == 0;
#else
  return ::setenv(name, value, /*overwrite=*/1) == 0;
#endif
}

bool remove_environment(const char* name) {
#ifdef _WIN32
  return ::_putenv_s(name, "") == 0;
#else
  return ::unsetenv(name) == 0;
#endif
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  {
    int connect_calls = 0;
    int spawn_calls = 0;
    cgraph::ClientRuntimeHooks hooks;
    hooks.connect = [&](const cgraph::DaemonIdentity&, const nlohmann::json&) {
      ++connect_calls;
      return ok_response("existing");
    };
    hooks.spawn = [&](const cgraph::DaemonIdentity&) {
      ++spawn_calls;
      return true;
    };

    cgraph::ClientRequest request{
        .project_root = std::filesystem::current_path(),
        .operation = "status",
        .max_connect_attempts = 3,
        .initial_backoff = 1ms,
    };
    const auto result = cgraph::send_thin_client_request(request, hooks);
    if (!result.response || (*result.response)["source"] != "existing" || connect_calls != 1 || spawn_calls != 0) {
      return 1;
    }
  }

  {
    int connect_calls = 0;
    int spawn_calls = 0;
    std::vector<std::chrono::milliseconds> sleeps;
    cgraph::ClientRuntimeHooks hooks;
    hooks.connect = [&](const cgraph::DaemonIdentity&, const nlohmann::json&) -> std::optional<nlohmann::json> {
      ++connect_calls;
      if (connect_calls >= 4) {
        return ok_response("spawned");
      }
      return std::nullopt;
    };
    hooks.spawn = [&](const cgraph::DaemonIdentity&) {
      ++spawn_calls;
      return true;
    };
    hooks.sleep = [&](std::chrono::milliseconds delay) {
      sleeps.push_back(delay);
    };

    cgraph::ClientRequest request{
        .project_root = std::filesystem::current_path(),
        .operation = "query",
        .params = nlohmann::json{{"q", "Alpha"}},
        .max_connect_attempts = 4,
        .initial_backoff = 5ms,
    };
    const auto result = cgraph::send_thin_client_request(request, hooks);
    if (!result.response || (*result.response)["source"] != "spawned" || spawn_calls != 1) {
      return 1;
    }
    if (sleeps.size() != 2 || sleeps[0] != 5ms || sleeps[1] != 10ms) {
      return 1;
    }
  }

  {
    std::atomic<int> spawn_calls{0};
    std::atomic<bool> spawned{false};
    cgraph::ClientRuntimeHooks hooks;
    hooks.connect = [&](const cgraph::DaemonIdentity&, const nlohmann::json&) -> std::optional<nlohmann::json> {
      if (spawned.load()) {
        return ok_response("shared");
      }
      return std::nullopt;
    };
    hooks.spawn = [&](const cgraph::DaemonIdentity&) {
      ++spawn_calls;
      std::this_thread::sleep_for(10ms);
      spawned.store(true);
      return true;
    };
    hooks.sleep = [](std::chrono::milliseconds) {};

    cgraph::ClientRequest request{
        .project_root = std::filesystem::current_path(),
        .operation = "status",
        .max_connect_attempts = 4,
        .initial_backoff = 1ms,
    };

    std::vector<std::thread> threads;
    std::atomic<int> successes{0};
    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&] {
        const auto result = cgraph::send_thin_client_request(request, hooks);
        if (result.response && (*result.response)["source"] == "shared") {
          ++successes;
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }

    if (spawn_calls.load() != 1 || successes.load() != 4) {
      return 1;
    }
  }

#ifndef _WIN32
  // A workspace asked while one repo's daemon is still building must not answer
  // as if that repo had nothing to say. `web`'s build is held on a real FIFO in
  // semantic replay (its snapshot stays empty/building). Real daemons, no fakes.
  {
    namespace fs = std::filesystem;
    const auto ws = fs::temp_directory_path() / "cgraph_client_runtime_cold_workspace";
    fs::remove_all(ws);
    const auto api = ws / "api";
    const auto web = ws / "web";
    write_file(api / "src" / "server.ts",
               "import express from 'express';\n"
               "const app = express();\n"
               "app.get('/api/v1/stats', (req, res) => res.json({ total: 1 }));\n");
    write_file(web / "src" / "stats.ts",
               "export async function loadStats() {\n"
               "  return fetch('/api/v1/stats');\n"
               "}\n");
    write_file(ws / std::string(cgraph::kWorkspaceFile),
               nlohmann::json{{"repos", nlohmann::json::array({{{"name", "api"}, {"root", api.generic_string()}},
                                                               {{"name", "web"}, {"root", web.generic_string()}}})}}
                   .dump());
    const auto drop_dir = web / "cgraph-out" / "semantic-drop";
    const auto barrier = drop_dir / "hold.fifo";
    fs::create_directories(drop_dir);
    if (::mkfifo(barrier.c_str(), 0600) != 0) {
      return 1;
    }
    write_file(drop_dir / "plan.json",
               R"({"chunks":[{"index":0,"inputs":[{"path":")" + barrier.generic_string() + R"(","content_hash":""}]}]})");
    write_file(drop_dir / "chunk_00.json", R"({"nodes":[],"edges":[],"hyperedges":[]})");

    cgraph::DaemonServerOptions options;
    options.idle_timeout = std::chrono::seconds(60);
    options.build_graph_on_start = true;
    options.code_poll_interval = std::chrono::milliseconds(0);
    options.drop_poll_interval = std::chrono::milliseconds(20);
    std::thread api_server([&] { (void)cgraph::run_daemon_server(api, options); });
    std::thread web_server([&] { (void)cgraph::run_daemon_server(web, options); });
    int writer = -1;
    std::thread release;
    // Every exit from this block stops both daemons and joins every thread.
    const auto finish = [&](bool passed) {
      if (writer >= 0) {
        (void)::write(writer, "x", 1);
        ::close(writer);
        writer = -1;
      }
      if (release.joinable()) {
        release.join();
      }
      for (const auto& root : {api, web}) {
        cgraph::ClientRequest stop{.project_root = root, .operation = "shutdown"};
        (void)cgraph::send_thin_client_request(stop, cgraph::default_client_runtime_hooks(stop));
      }
      api_server.join();
      web_server.join();
      fs::remove_all(ws);
      return passed;
    };

    // Wait until web's build is parked on the FIFO, so every ask below is cold.
    for (int attempt = 0; attempt < 500 && writer < 0; ++attempt) {
      writer = ::open(barrier.c_str(), O_WRONLY | O_NONBLOCK);
      if (writer < 0) {
        std::this_thread::sleep_for(20ms);
      }
    }
    if (writer < 0) {
      (void)finish(false);
      return 1;
    }

    const nlohmann::json params{{"id", "endpoint:GET /api/v1/stats"}, {"direction", "dependents"}, {"max_depth", 3}};
    const auto ask = [&](std::chrono::milliseconds wait) {
      cgraph::ClientRequest request{.project_root = ws, .operation = "impact", .params = params};
      request.build_wait = wait;
      return cgraph::send_thin_client_request(request, cgraph::default_client_runtime_hooks(request));
    };
    const auto reached_web = [](const cgraph::ClientResult& result) {
      if (!result.response || !result.response->value("ok", false)) {
        return false;
      }
      for (const auto& node : (*result.response)["result"].value("nodes", nlohmann::json::array())) {
        if (node.value("repo", std::string{}) == "web") {
          return true;
        }
      }
      return false;
    };
    const auto names_web_building = [](const cgraph::ClientResult& result) {
      return result.response && (*result.response)["result"].contains("building") &&
             (*result.response)["result"]["building"].dump().find("\"web\"") != std::string::npos;
    };

    // No wait: the building answer comes back at once, marked, without web.
    const auto immediate = ask(0ms);
    if (!names_web_building(immediate) || reached_web(immediate)) {
      std::cerr << "zero wait: " << (immediate.response ? immediate.response->dump() : immediate.error) << '\n';
      (void)finish(false);
      return 1;
    }
    // A wait that runs out bounds the whole federated request (first pass and
    // every contract hop), not each member ask: about one wait, never several.
    const auto started = std::chrono::steady_clock::now();
    // 1.5 s total; a wait per ask would take at least 3 s (first pass plus one
    // contract hop), so the 2.4 s ceiling separates the two by a wide margin.
    const auto bounded = ask(1500ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!names_web_building(bounded) || elapsed > 2400ms) {
      std::cerr << "bounded wait took " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                << " ms: " << (bounded.response ? bounded.response->dump() : bounded.error) << '\n';
      (void)finish(false);
      return 1;
    }
    // The default wait outlasts a build that publishes shortly: web's consumer
    // is in the answer and nothing is marked building.
    release = std::thread([&] {
      std::this_thread::sleep_for(400ms);
      (void)::write(writer, "x", 1);
      ::close(writer);
      writer = -1;
    });
    const auto settled = ask(std::chrono::milliseconds(30000));
    release.join();
    const bool passed = reached_web(settled) && !(*settled.response)["result"].contains("building");
    if (!passed) {
      std::cerr << "cold workspace impact: " << (settled.response ? settled.response->dump() : settled.error) << '\n';
    }
    if (!finish(passed)) {
      return 1;
    }
  }
#endif

#ifndef _WIN32
  // Opened in one service repo, an agent sees the other services where a change
  // crosses into them: `impact` from the api member root reaches web's caller,
  // `query` stays in api, and change_context on an edit inside the api handler
  // lists web's consumer in `cross_service`. Real daemons, no fakes.
  {
    namespace fs = std::filesystem;
    const auto ws = fs::temp_directory_path() / "cgraph_client_runtime_member_workspace";
    fs::remove_all(ws);
    const auto api = ws / "api";
    const auto web = ws / "web";
    const std::string handler_before =
        "import express from 'express';\n"
        "const app = express();\n"
        "app.get('/api/v1/stats', (req, res) => {\n"
        "  const total = 1;\n"
        "  res.json({ total });\n"
        "});\n";
    std::string handler_after = handler_before;
    handler_after.replace(handler_after.find("total = 1"), 9, "total = 2");
    write_file(api / "src" / "server.ts", handler_after);
    write_file(ws / "api-base" / "src" / "server.ts", handler_before);
    write_file(web / "src" / "stats.ts",
               "export async function loadStats() {\n"
               "  return fetch('/api/v1/stats');\n"
               "}\n");
    write_file(ws / std::string(cgraph::kWorkspaceFile),
               R"({"name": "shop", "repos": [{"name": "api", "root": "./api"}, {"name": "web", "root": "./web"}]})");
    write_file(ws / "change.diff",
               "--- a/src/server.ts\n+++ b/src/server.ts\n@@ -3,4 +3,4 @@\n"
               " app.get('/api/v1/stats', (req, res) => {\n-  const total = 1;\n+  const total = 2;\n"
               "   res.json({ total });\n });\n");

    cgraph::DaemonServerOptions options;
    options.idle_timeout = std::chrono::seconds(60);
    options.build_graph_on_start = true;
    options.code_poll_interval = std::chrono::milliseconds(0);
    std::thread api_server([&] { (void)cgraph::run_daemon_server(api, options); });
    std::thread web_server([&] { (void)cgraph::run_daemon_server(web, options); });
    const auto finish = [&](bool passed) {
      for (const auto& root : {api, web}) {
        cgraph::ClientRequest stop{.project_root = root, .operation = "shutdown"};
        stop.federate = false;
        (void)cgraph::send_thin_client_request(stop, cgraph::default_client_runtime_hooks(stop));
      }
      api_server.join();
      web_server.join();
      fs::remove_all(ws);
      return passed;
    };
    const auto ask = [](const fs::path& root, const std::string& op, const nlohmann::json& params) {
      cgraph::ClientRequest request{.project_root = root, .operation = op, .params = params};
      return cgraph::send_thin_client_request(request, cgraph::default_client_runtime_hooks(request));
    };

    const auto impact = ask(api, "impact", {{"id", "endpoint:GET /api/v1/stats"}, {"direction", "dependents"}, {"max_depth", 3}});
    bool web_caller = false;
    if (impact.response && impact.response->value("ok", false)) {
      for (const auto& node : (*impact.response)["result"].value("nodes", nlohmann::json::array())) {
        web_caller = web_caller || (node.value("repo", std::string{}) == "web" && node.value("label", std::string{}) == "loadStats");
      }
    }
    const bool tagged = impact.response && (*impact.response)["result"].value("workspace", nlohmann::json::object())
                                                   .value("home", std::string{}) == "api";
    if (!web_caller || !tagged) {
      std::cerr << "member impact: " << (impact.response ? impact.response->dump() : impact.error) << '\n';
      (void)finish(false);
      return 1;
    }
    const auto query = ask(api, "query", {{"q", "loadStats"}});
    if (!query.response || query.response->dump().find("loadStats") != std::string::npos) {
      std::cerr << "member query left the home repo: " << (query.response ? query.response->dump() : query.error) << '\n';
      (void)finish(false);
      return 1;
    }

    nlohmann::json context;
    try {
      context = cgraph::change_context_across_workspace(
          {{"base_root", (ws / "api-base").generic_string()}, {"target_root", api.generic_string()},
           {"diff_path", (ws / "change.diff").generic_string()}},
          cgraph::ClientRequest{});
    } catch (const std::exception& error) {
      std::cerr << "change_context: " << error.what() << '\n';
      (void)finish(false);
      return 1;
    }
    bool consumer = false;
    for (const auto& row : context.value("cross_service", nlohmann::json::object()).value("rows", nlohmann::json::array())) {
      consumer = consumer || (row.value("repo", std::string{}) == "web" && row.value("relation", std::string{}) == "consumer" &&
                              row.value("path", std::string{}) == "src/stats.ts" && row.value("label", std::string{}) == "loadStats");
    }
    const auto contracts = context.value("cross_service", nlohmann::json::object()).value("contracts", nlohmann::json::array());
    if (!consumer || contracts.size() != 1 || contracts[0].value("id", std::string{}) != "endpoint:GET /api/v1/stats") {
      std::cerr << "cross_service: " << context.value("cross_service", nlohmann::json{}).dump() << '\n';
      (void)finish(false);
      return 1;
    }
    if (!finish(true)) {
      return 1;
    }
  }
#endif

  // Daemon discovery precedence: an explicit path wins, then CGRAPH_DAEMON_PATH,
  // then a graphd beside the executable (absent for this test binary -> empty).
  {
    if (!remove_environment("CGRAPH_DAEMON_PATH")) {
      return 1;
    }
    if (cgraph::resolve_daemon_path("/explicit/graphd") != std::filesystem::path{"/explicit/graphd"}) {
      return 1;
    }
    if (!set_environment("CGRAPH_DAEMON_PATH", "/env/graphd")) {
      return 1;
    }
    if (cgraph::resolve_daemon_path({}) != std::filesystem::path{"/env/graphd"}) {
      return 1;
    }
    if (cgraph::resolve_daemon_path("/explicit/graphd") != std::filesystem::path{"/explicit/graphd"}) {
      return 1;  // explicit beats the environment
    }
    if (!remove_environment("CGRAPH_DAEMON_PATH")) {
      return 1;
    }
    if (!cgraph::resolve_daemon_path({}).empty()) {
      return 1;  // no graphd ships next to the test binary
    }
  }

  return 0;
}
