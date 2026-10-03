// End-to-end coverage of the cgraph-mcp stdio loop (src/mcp/main.cpp): a
// parseable line of the wrong JSON-RPC shape is answered with Invalid Request
// (-32600; the request's id when readable, else null) and the loop keeps serving the next line. Before, nlohmann
// threw on the first typed read and the uncaught exception aborted the server.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

void expect(bool& ok, bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

struct RunResult {
  int exit_code = -1;
  std::string stdout_text;
};

// Runs `program args...` with `input` on stdin; returns its exit code and stdout.
RunResult run_with_stdin(const std::string& program, std::vector<std::string> args, const std::string& input) {
  int to_child[2];
  int from_child[2];
  if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
    return {};
  }
  const pid_t child = ::fork();
  if (child == 0) {
    ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    std::vector<char*> argv;
    std::string name = program;
    argv.push_back(name.data());
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    ::execv(name.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(to_child[0]);
  ::close(from_child[1]);
  // The input is far below the pipe buffer, so writing it all before reading cannot deadlock.
  (void)::write(to_child[1], input.data(), input.size());
  ::close(to_child[1]);
  RunResult result;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(from_child[0], buffer, sizeof(buffer));
    if (count <= 0) {
      break;
    }
    result.stdout_text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(from_child[0]);
  int status = 0;
  ::waitpid(child, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: cgraph_mcp_main_test <cgraph-mcp binary>\n";
    return 2;
  }
  bool ok = true;
  const auto root = std::filesystem::temp_directory_path();

  // Three malformed-but-parseable lines, then a valid request that needs no daemon.
  const std::string input =
      "5\n"
      "[1]\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":7}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}\n";
  const auto run = run_with_stdin(argv[1], {"--root", root.string()}, input);
  expect(ok, run.exit_code == 0, "cgraph-mcp exits 0 at end of input (got " + std::to_string(run.exit_code) + ")");

  std::vector<nlohmann::json> responses;
  std::istringstream lines(run.stdout_text);
  for (std::string line; std::getline(lines, line);) {
    responses.push_back(nlohmann::json::parse(line, nullptr, false));
  }
  expect(ok, responses.size() == 4, "one response per request line: " + run.stdout_text);
  // `5` and `[1]` carry no readable id (null); the object's id 3 is echoed.
  const std::vector<nlohmann::json> expected_ids{nullptr, nullptr, 3};
  for (std::size_t index = 0; index < expected_ids.size() && index < responses.size(); ++index) {
    const auto& response = responses[index];
    expect(ok,
           response.is_object() && response.value("jsonrpc", std::string{}) == "2.0" &&
               response["id"] == expected_ids[index] && response["error"]["code"] == -32600,
           "malformed line " + std::to_string(index + 1) + " answered with Invalid Request, id " +
               expected_ids[index].dump() + ": " + response.dump());
  }
  if (responses.size() == 4) {
    const auto& listed = responses[3];
    expect(ok, listed.is_object() && listed["id"] == 9 && !listed["result"]["tools"].empty(),
           "the valid request after the malformed lines is still answered: " + listed.dump());
  }

  if (ok) {
    std::cout << "mcp_main_test: ok\n";
  }
  return ok ? 0 : 1;
}
