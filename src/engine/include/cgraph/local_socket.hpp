#pragma once

// Local stream-socket seam for the daemon endpoint. Both platforms speak
// AF_UNIX SOCK_STREAM at a filesystem path: BSD sockets on POSIX, Winsock with
// <afunix.h> on Windows (10 1803 and later). Every Winsock/BSD difference
// (handle type, error codes, close, non-blocking mode, poll, timeouts,
// SIGPIPE, socket-file removal) lives behind these functions, so the daemon
// server and client share one framing and serve-loop code path.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace cgraph {

#ifdef _WIN32
using SocketHandle = std::uintptr_t;  // SOCKET
inline constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0);
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

using SocketDeadline = std::optional<std::chrono::steady_clock::time_point>;

// Owns one socket handle; closes it on destruction.
class LocalSocket {
 public:
  LocalSocket() = default;
  explicit LocalSocket(SocketHandle handle) : handle_(handle) {}
  LocalSocket(const LocalSocket&) = delete;
  LocalSocket& operator=(const LocalSocket&) = delete;
  LocalSocket(LocalSocket&& other) noexcept : handle_(other.release()) {}
  LocalSocket& operator=(LocalSocket&& other) noexcept;
  ~LocalSocket();

  [[nodiscard]] bool valid() const { return handle_ != kInvalidSocket; }
  [[nodiscard]] SocketHandle get() const { return handle_; }
  SocketHandle release();

 private:
  SocketHandle handle_ = kInvalidSocket;
};

// Description of the last socket error on this thread (errno / WSAGetLastError).
[[nodiscard]] std::string last_socket_error();

// A bound, listening socket at `path`. The caller has already removed any
// stale socket file. Logs nothing; returns an invalid socket on failure with
// `error` describing which step failed.
[[nodiscard]] LocalSocket listen_on_local_socket(const std::filesystem::path& path, std::string& error);

// Connects to the listener at `path`. With a deadline the connect never parks
// on a full backlog and gives up once the deadline passes. Returns an invalid
// socket when nothing accepts (absent path, stale file, refused).
[[nodiscard]] LocalSocket connect_local_socket(const std::filesystem::path& path,
                                               const SocketDeadline& deadline = std::nullopt);

enum class SocketWait { Ready, TimedOut, Failed };

// Waits up to `timeout` for `listener` to have a pending connection.
[[nodiscard]] SocketWait wait_for_connection(const LocalSocket& listener, std::chrono::milliseconds timeout);

// Accepts one pending connection, with per-call send/receive timeouts of
// `io_timeout` so a stalled peer cannot wedge the caller. Invalid on failure.
[[nodiscard]] LocalSocket accept_local_connection(const LocalSocket& listener, std::chrono::seconds io_timeout);

// Sends/receives exactly `size` bytes. With a deadline, nothing blocks past it.
// A peer that hung up is a failed send, never a process-killing SIGPIPE.
[[nodiscard]] bool send_all(const LocalSocket& socket, const std::uint8_t* data, std::size_t size,
                            const SocketDeadline& deadline = std::nullopt);
[[nodiscard]] bool receive_exact(const LocalSocket& socket, std::uint8_t* data, std::size_t size,
                                 const SocketDeadline& deadline = std::nullopt);

// Whether the peer has already closed its end (checked without blocking).
[[nodiscard]] bool peer_hung_up(const LocalSocket& socket);

// Removes a socket file. True when it is gone (removed, or never existed).
// On Windows an AF_UNIX socket file is a reparse point that std::filesystem
// status queries cannot follow, so this goes straight to the OS delete call.
[[nodiscard]] bool remove_socket_file(const std::filesystem::path& path);

// Longest socket path (in bytes) this platform's sockaddr_un can hold.
[[nodiscard]] std::size_t max_socket_path_bytes();

}  // namespace cgraph
