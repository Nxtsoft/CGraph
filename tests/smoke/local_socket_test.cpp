#include "cgraph/local_socket.hpp"

#include "cgraph/daemon_endpoint.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

int fail(const std::string& message) {
  std::cerr << "local_socket_test: " << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;
  // The runtime dir keeps the path inside sun_path on every platform (a long
  // temp dir on macOS would not).
  const auto path = cgraph::unix_runtime_dir() / "local-socket-test.sock";
  cgraph::ensure_unix_socket_dir(path);
  (void)cgraph::remove_socket_file(path);

  // Nothing listening: an absent path refuses at once, with or without a deadline.
  if (cgraph::connect_local_socket(path).valid()) return fail("connected to an absent path");
  const auto started = std::chrono::steady_clock::now();
  if (cgraph::connect_local_socket(path, started + 2s).valid()) return fail("deadline connect to an absent path");
  if (std::chrono::steady_clock::now() - started > 1s) return fail("refused connect waited for the deadline");

  std::string error;
  if (cgraph::listen_on_local_socket(std::filesystem::path(std::string(cgraph::max_socket_path_bytes() + 1, 'x')),
                                     error)
          .valid() ||
      error.find("too long") == std::string::npos) {
    return fail("overlong path not rejected: " + error);
  }

  const auto listener = cgraph::listen_on_local_socket(path, error);
  if (!listener.valid()) return fail("listen: " + error);
  if (cgraph::wait_for_connection(listener, 50ms) != cgraph::SocketWait::TimedOut) {
    return fail("idle listener did not time out");
  }

  // A full exchange in both directions, the client bound by a deadline (the
  // non-blocking path the thin client takes).
  std::array<std::uint8_t, 5> request{'h', 'e', 'l', 'l', 'o'};
  std::array<std::uint8_t, 3> reply{'o', 'k', '!'};
  std::string client_error;
  std::thread client([&] {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto socket = cgraph::connect_local_socket(path, deadline);
    if (!socket.valid()) {
      client_error = "client connect failed: " + cgraph::last_socket_error();
      return;
    }
    std::array<std::uint8_t, 3> got{};
    if (!cgraph::send_all(socket, request.data(), request.size(), deadline) ||
        !cgraph::receive_exact(socket, got.data(), got.size(), deadline) || got != reply) {
      client_error = "client exchange failed";
    }
  });
  if (cgraph::wait_for_connection(listener, 5s) != cgraph::SocketWait::Ready) {
    client.join();
    return fail("no pending connection");
  }
  {
    const auto conn = cgraph::accept_local_connection(listener, 5s);
    if (!conn.valid()) {
      client.join();
      return fail("accept failed");
    }
    std::array<std::uint8_t, 5> got{};
    if (!cgraph::receive_exact(conn, got.data(), got.size()) || got != request) {
      client.join();
      return fail("server did not receive the request");
    }
    if (cgraph::peer_hung_up(conn)) {
      client.join();
      return fail("live peer reported hung up");
    }
    if (!cgraph::send_all(conn, reply.data(), reply.size())) {
      client.join();
      return fail("server send failed");
    }
    client.join();
    if (!client_error.empty()) return fail(client_error);
    // The client has closed its socket: the server sees the hang-up.
    bool hung_up = false;
    for (int attempt = 0; attempt < 100 && !hung_up; ++attempt) {
      hung_up = cgraph::peer_hung_up(conn);
      if (!hung_up) std::this_thread::sleep_for(10ms);
    }
    if (!hung_up) return fail("closed peer not reported hung up");
    std::array<std::uint8_t, 1> more{};
    if (cgraph::receive_exact(conn, more.data(), more.size())) return fail("read past a closed peer");
  }

  if (!cgraph::remove_socket_file(path)) return fail("remove_socket_file failed");
  if (!cgraph::remove_socket_file(path)) return fail("removing an absent socket file failed");
  return 0;
}
