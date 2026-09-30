#include "cgraph/client_runtime.hpp"

#include "cgraph/change_context.hpp"
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
#include <set>
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
  // crosses into them. Real daemons for `api` and `web`; `billing` is listed but
  // has no daemon and none can be spawned here, so it must be named unreachable.
  {
    namespace fs = std::filesystem;
    const auto ws = fs::temp_directory_path() / "cgraph_client_runtime_member_workspace";
    fs::remove_all(ws);
    const auto api = ws / "api";
    const auto web = ws / "web";
    // An Elysia router chain spans its whole file, and the app that mounts it
    // serves a route of its own: the shape that makes every mounted route
    // reachable from an edit to one handler.
    const std::string routes_before =
        "import { Elysia } from 'elysia';\n"
        "export const statsRoutes = new Elysia({ prefix: '/stats' })\n"
        "  .get('/', () => {\n"
        "    const total = 1;\n"
        "    return { total };\n"
        "  })\n"
        "  .get('/health', () => {\n"
        "    return { ok: true };\n"
        "  });\n";
    const std::string server_before =
        "import { Elysia } from 'elysia';\n"
        "import { statsRoutes } from './routes/stats';\n"
        "export const app = new Elysia({ prefix: '/api/v1' })\n"
        "  .use(statsRoutes)\n"
        "  .get('/ping', () => {\n"
        "    return { pong: true };\n"
        "  });\n";
    const std::string web_before =
        "export async function loadStats() {\n"
        "  return fetch('/api/v1/stats');\n"
        "}\n"
        "export async function loadHealth() {\n"
        "  return fetch('/api/v1/stats/health');\n"
        "}\n"
        "export async function loadPing() {\n"
        "  return fetch('/api/v1/ping');\n"
        "}\n";
    for (const auto& root : {api, ws / "api-base"}) {
      write_file(root / "src" / "routes" / "stats.ts", routes_before);
      write_file(root / "src" / "server.ts", server_before);
    }
    for (const auto& root : {web, ws / "web-base"}) write_file(root / "src" / "stats.ts", web_before);
    fs::create_directories(ws / "billing");
    // billing must be unreachable whatever the environment: with no graphd to
    // start, asking it fails. Restored when the block ends.
    const char* daemon_env = std::getenv("CGRAPH_DAEMON_PATH");
    const std::string saved_daemon = daemon_env == nullptr ? "" : daemon_env;
    (void)remove_environment("CGRAPH_DAEMON_PATH");
    write_file(ws / std::string(cgraph::kWorkspaceFile),
               R"({"name": "shop", "repos": [{"name": "api", "root": "./api"}, {"name": "web", "root": "./web"},
                                             {"name": "billing", "root": "./billing"}]})");

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
      if (daemon_env != nullptr) (void)set_environment("CGRAPH_DAEMON_PATH", saved_daemon.c_str());
      return passed;
    };
    const auto fail = [&](const std::string& what, const nlohmann::json& got) {
      std::cerr << what << ": " << got.dump() << '\n';
      (void)finish(false);
      return 1;
    };
    const auto ask = [](const fs::path& root, const std::string& op, const nlohmann::json& params, bool federate = true) {
      cgraph::ClientRequest request{.project_root = root, .operation = op, .params = params};
      request.federate = federate;
      const auto result = cgraph::send_thin_client_request(request, cgraph::default_client_runtime_hooks(request));
      return result.response ? *result.response : nlohmann::json{{"ok", false}, {"error", result.error}};
    };
    const auto has_node = [](const nlohmann::json& answer, const std::string& repo, const std::string& label) {
      for (const auto& node : answer.value("result", nlohmann::json::object()).value("nodes", nlohmann::json::array())) {
        if (node.value("repo", std::string{}) == repo && node.value("label", std::string{}) == label) return true;
      }
      return false;
    };
    const auto rows_of = [](const nlohmann::json& context) {
      return context.value("cross_service", nlohmann::json::object()).value("rows", nlohmann::json::array());
    };
    const auto has_row = [&](const nlohmann::json& context, const std::string& relation, const std::string& repo,
                             const std::string& path, const std::string& label) {
      for (const auto& row : rows_of(context)) {
        if (row.value("relation", std::string{}) == relation && row.value("repo", std::string{}) == repo &&
            row.value("path", std::string{}) == path && row.value("label", std::string{}) == label) return true;
      }
      return false;
    };
    const auto contract_ids = [](const nlohmann::json& context) {
      std::set<std::string> ids;
      for (const auto& contract : context.value("cross_service", nlohmann::json::object()).value("contracts", nlohmann::json::array()))
        ids.insert(contract.value("id", std::string{}));
      return ids;
    };
    const auto names_billing_once = [](const nlohmann::json& context) {
      const auto list = context.value("cross_service", nlohmann::json::object()).value("unreachable", nlohmann::json::array());
      return list.size() == 1 && list[0].value("repo", std::string{}) == "billing";
    };
    const auto run_change = [&](const fs::path& base, const fs::path& target, const std::string& diff, long budget = 6000) {
      write_file(ws / "change.diff", diff);
      return cgraph::change_context_across_workspace(
          {{"base_root", base.generic_string()}, {"target_root", target.generic_string()},
           {"diff_path", (ws / "change.diff").generic_string()}, {"budget", budget}},
          cgraph::ClientRequest{});
    };

    // impact from the api member root crosses into web; query stays home.
    const auto impact = ask(api, "impact", {{"id", "endpoint:GET /api/v1/stats"}, {"direction", "dependents"}, {"max_depth", 3}});
    if (!has_node(impact, "web", "loadStats") ||
        impact["result"].value("workspace", nlohmann::json::object()).value("home", std::string{}) != "api") {
      return fail("member impact", impact);
    }
    const auto query = ask(api, "query", {{"q", "loadStats"}});
    if (query.dump().find("loadStats") != std::string::npos) return fail("member query left home", query);

    // A pin names the home graph: a wrong one fails the request, the right one
    // still crosses into web.
    const auto lone = ask(api, "impact", {{"id", "endpoint:GET /api/v1/stats"}}, false);
    const auto pin = lone.value("result", nlohmann::json::object()).value("freshness", nlohmann::json::object())
                         .value("content_root", std::string{});
    const auto wrong = ask(api, "impact", {{"id", "endpoint:GET /api/v1/stats"}, {"direction", "dependents"},
                                           {"expected_content_root", std::string(64, '0')}});
    if (pin.empty() || wrong.value("ok", true)) return fail("wrong pin was accepted", wrong);
    const auto pinned = ask(api, "impact", {{"id", "endpoint:GET /api/v1/stats"}, {"direction", "dependents"},
                                            {"max_depth", 3}, {"expected_content_root", pin}});
    if (!has_node(pinned, "web", "loadStats")) return fail("right pin lost web", pinned);

    nlohmann::json context;
    try {
      // An edit inside the stats handler: that route only, and only its caller.
      write_file(api / "src" / "routes" / "stats.ts",
                 std::string(routes_before).replace(routes_before.find("total = 1"), 9, "total = 2"));
      context = run_change(ws / "api-base", api,
                           "--- a/src/routes/stats.ts\n+++ b/src/routes/stats.ts\n@@ -3,4 +3,4 @@\n"
                           "   .get('/', () => {\n-    const total = 1;\n+    const total = 2;\n"
                           "     return { total };\n   })\n");
      if (contract_ids(context) != std::set<std::string>{"endpoint:GET /api/v1/stats"} ||
          !has_row(context, "consumer", "web", "src/stats.ts", "loadStats") ||
          has_row(context, "consumer", "web", "src/stats.ts", "loadHealth") ||
          has_row(context, "consumer", "web", "src/stats.ts", "loadPing") || !names_billing_once(context)) {
        return fail("handler edit", context["cross_service"]);
      }
      // A route that differs only in a file the diff does not supply is still
      // named, but marked outside_diff and ranked after the edited route.
      write_file(ws / "api-base" / "src" / "admin.ts",
                 "import { Elysia } from 'elysia';\n"
                 "export const admin = new Elysia().get('/api/v1/admin', () => {\n  return {};\n});\n");
      context = run_change(ws / "api-base", api,
                           "--- a/src/routes/stats.ts\n+++ b/src/routes/stats.ts\n@@ -3,4 +3,4 @@\n"
                           "   .get('/', () => {\n-    const total = 1;\n+    const total = 2;\n"
                           "     return { total };\n   })\n");
      fs::remove(ws / "api-base" / "src" / "admin.ts");
      {
        const auto contracts = context["cross_service"].value("contracts", nlohmann::json::array());
        const bool edited_first = !contracts.empty() && contracts[0].value("id", std::string{}) == "endpoint:GET /api/v1/stats";
        bool admin_marked = false;
        for (const auto& contract : contracts) {
          admin_marked = admin_marked || (contract.value("id", std::string{}) == "endpoint:GET /api/v1/admin" &&
                                          contract.value("outside_diff", false) && contract.value("rank", 0) == 3);
        }
        if (!edited_first || !admin_marked) return fail("unsupplied difference", context["cross_service"]);
      }
      // Moving the mount prefix serves neither route at its old path: both old
      // routes are removed contracts, and both web callers are named.
      write_file(api / "src" / "routes" / "stats.ts", routes_before);
      write_file(api / "src" / "server.ts",
                 std::string(server_before).replace(server_before.find("/api/v1"), 7, "/api/v2"));
      context = run_change(ws / "api-base", api,
                           "--- a/src/server.ts\n+++ b/src/server.ts\n@@ -2,3 +2,3 @@\n"
                           " import { statsRoutes } from './routes/stats';\n"
                           "-export const app = new Elysia({ prefix: '/api/v1' })\n"
                           "+export const app = new Elysia({ prefix: '/api/v2' })\n"
                           "   .use(statsRoutes)\n");
      if (!contract_ids(context).contains("endpoint:GET /api/v1/stats") ||
          !has_row(context, "consumer", "web", "src/stats.ts", "loadStats") ||
          !has_row(context, "consumer", "web", "src/stats.ts", "loadHealth")) {
        return fail("mount change", context["cross_service"]);
      }
      write_file(api / "src" / "server.ts", server_before);
      // From the caller's side: an edit inside loadStats names api's handler.
      write_file(web / "src" / "stats.ts",
                 std::string(web_before).replace(web_before.find("fetch('/api/v1/stats')"), 22,
                                                 "fetch('/api/v1/stats' )"));
      context = run_change(ws / "web-base", web,
                           "--- a/src/stats.ts\n+++ b/src/stats.ts\n@@ -1,3 +1,3 @@\n"
                           " export async function loadStats() {\n-  return fetch('/api/v1/stats');\n"
                           "+  return fetch('/api/v1/stats' );\n }\n");
      bool provider = false;
      for (const auto& row : rows_of(context)) {
        provider = provider || (row.value("relation", std::string{}) == "provider" && row.value("repo", std::string{}) == "api" &&
                                row.value("path", std::string{}) == "src/routes/stats.ts" &&
                                row.value("id", std::string{}) == "src_routes_stats_ts_statsroutes_get");
      }
      if (!provider) return fail("caller edit", context["cross_service"]);
      write_file(web / "src" / "stats.ts", web_before);
      // A budget the change alone fits must still fit with the section: it
      // shrinks, it never causes a rejection.
      write_file(api / "src" / "routes" / "stats.ts",
                 std::string(routes_before).replace(routes_before.find("total = 1"), 9, "total = 2"));
      const std::string handler_diff =
          "--- a/src/routes/stats.ts\n+++ b/src/routes/stats.ts\n@@ -3,4 +3,4 @@\n"
          "   .get('/', () => {\n-    const total = 1;\n+    const total = 2;\n"
          "     return { total };\n   })\n";
      long smallest = 0;
      for (long budget = 128; budget <= 4000 && smallest == 0; budget += 16) {
        write_file(ws / "change.diff", handler_diff);
        try {
          (void)cgraph::change_context({{"base_root", (ws / "api-base").generic_string()}, {"target_root", api.generic_string()},
                                        {"diff_path", (ws / "change.diff").generic_string()}, {"budget", budget}});
          smallest = budget;
        } catch (const std::exception&) {
        }
      }
      if (smallest == 0) return fail("no budget fits the change alone", nlohmann::json{});
      context = run_change(ws / "api-base", api, handler_diff, smallest);
      if (!context.contains("omitted") || context.value("tokens_used", 0L) > smallest) {
        return fail("small budget", context);
      }
    } catch (const std::exception& error) {
      return fail(std::string("change_context threw: ") + error.what(), context);
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
