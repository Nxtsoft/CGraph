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
  // semantic replay (its snapshot stays empty/building); the client has to wait
  // for it and return web's consumer of the endpoint. Real daemons, no fakes.
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

    // Wait until web's build is parked on the FIFO, so the first ask is cold.
    int writer = -1;
    for (int attempt = 0; attempt < 500 && writer < 0; ++attempt) {
      writer = ::open(barrier.c_str(), O_WRONLY | O_NONBLOCK);
      if (writer < 0) {
        std::this_thread::sleep_for(20ms);
      }
    }
    if (writer < 0) {
      return 1;
    }
    std::thread release([&] {
      std::this_thread::sleep_for(400ms);
      (void)::write(writer, "x", 1);
      ::close(writer);
    });

    cgraph::ClientRequest request{
        .project_root = ws,
        .operation = "impact",
        .params = {{"id", "endpoint:GET /api/v1/stats"}, {"direction", "dependents"}, {"max_depth", 3}},
    };
    const auto result = cgraph::send_thin_client_request(request, cgraph::default_client_runtime_hooks(request));
    release.join();

    bool web_reached = false;
    if (result.response && (*result.response).value("ok", false)) {
      const auto& answer = (*result.response)["result"];
      for (const auto& node : answer.value("nodes", nlohmann::json::array())) {
        web_reached = web_reached || node.value("repo", std::string{}) == "web";
      }
      if (answer.contains("building")) {
        web_reached = false;  // answered from a graph that was still building
      }
    }
    for (const auto& root : {api, web}) {
      cgraph::ClientRequest stop{.project_root = root, .operation = "shutdown"};
      (void)cgraph::send_thin_client_request(stop, cgraph::default_client_runtime_hooks(stop));
    }
    api_server.join();
    web_server.join();
    fs::remove_all(ws);
    if (!web_reached) {
      std::cerr << "cold workspace impact: " << (result.response ? result.response->dump() : result.error) << '\n';
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
