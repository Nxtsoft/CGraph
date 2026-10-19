#include "cgraph/local_socket.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace cgraph {
namespace {

#ifdef _WIN32

using PollEntry = WSAPOLLFD;
using SockLen = int;
constexpr short kReadable = POLLRDNORM;
constexpr short kWritable = POLLWRNORM;

// Winsock must be started once per process before any socket call; the
// matching WSACleanup is left to process exit.
[[nodiscard]] bool winsock_ready() {
  static const bool ready = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

[[nodiscard]] int last_error_code() { return ::WSAGetLastError(); }
// Winsock has no signal interruption for these calls; nothing to retry.
[[nodiscard]] bool interrupted() { return false; }
[[nodiscard]] int poll_entries(PollEntry* entries, unsigned count, int timeout_ms) {
  return ::WSAPoll(entries, count, timeout_ms);
}
void close_handle(SocketHandle handle) { ::closesocket(static_cast<SOCKET>(handle)); }
[[nodiscard]] SocketHandle new_stream_socket() {
  if (!winsock_ready()) {
    return kInvalidSocket;
  }
  const SOCKET handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
  return handle == INVALID_SOCKET ? kInvalidSocket : static_cast<SocketHandle>(handle);
}
[[nodiscard]] long send_some(SocketHandle handle, const std::uint8_t* data, std::size_t size) {
  const int chunk = static_cast<int>(std::min<std::size_t>(size, 1U << 30U));
  return ::send(static_cast<SOCKET>(handle), reinterpret_cast<const char*>(data), chunk, 0);
}
[[nodiscard]] long receive_some(SocketHandle handle, std::uint8_t* data, std::size_t size) {
  const int chunk = static_cast<int>(std::min<std::size_t>(size, 1U << 30U));
  return ::recv(static_cast<SOCKET>(handle), reinterpret_cast<char*>(data), chunk, 0);
}
[[nodiscard]] bool set_non_blocking(SocketHandle handle) {
  u_long on = 1;
  return ::ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &on) == 0;
}
[[nodiscard]] bool set_io_timeout(SocketHandle handle, std::chrono::seconds timeout) {
  // Winsock takes the timeout as a DWORD of milliseconds, not a timeval.
  const DWORD ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
  const auto* value = reinterpret_cast<const char*>(&ms);
  return ::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_RCVTIMEO, value, sizeof(ms)) == 0 &&
         ::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_SNDTIMEO, value, sizeof(ms)) == 0;
}
// A non-blocking connect that has not completed yet (finished by waiting for
// writability below). Linux and macOS complete or refuse a local connect at
// once, and EAGAIN there means a full backlog: fail fast, as before.
[[nodiscard]] bool connect_in_progress(int code) { return code == WSAEWOULDBLOCK; }
[[nodiscard, maybe_unused]] bool peek_saw_eof(SocketHandle handle) {
  char byte = 0;
  return ::recv(static_cast<SOCKET>(handle), &byte, 1, MSG_PEEK) == 0;
}

#else  // POSIX

using PollEntry = pollfd;
using SockLen = socklen_t;
constexpr short kReadable = POLLIN;
constexpr short kWritable = POLLOUT;

[[nodiscard]] int last_error_code() { return errno; }
[[nodiscard]] bool interrupted() { return errno == EINTR; }
[[nodiscard]] int poll_entries(PollEntry* entries, unsigned count, int timeout_ms) {
  return ::poll(entries, static_cast<nfds_t>(count), timeout_ms);
}
void close_handle(SocketHandle handle) { ::close(handle); }

// Where send() has no MSG_NOSIGNAL (macOS), the socket itself must not raise
// SIGPIPE on a write to a peer that hung up.
void suppress_sigpipe(SocketHandle handle) {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
  const int on = 1;
  (void)::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
  (void)handle;
#endif
}
[[nodiscard]] SocketHandle new_stream_socket() {
  const int handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (handle >= 0) {
    suppress_sigpipe(handle);
  }
  return handle < 0 ? kInvalidSocket : handle;
}
[[nodiscard]] long send_some(SocketHandle handle, const std::uint8_t* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
  return static_cast<long>(::send(handle, data, size, MSG_NOSIGNAL));
#else
  return static_cast<long>(::send(handle, data, size, 0));
#endif
}
[[nodiscard]] long receive_some(SocketHandle handle, std::uint8_t* data, std::size_t size) {
  return static_cast<long>(::recv(handle, data, size, 0));
}
[[nodiscard]] bool set_non_blocking(SocketHandle handle) {
  return ::fcntl(handle, F_SETFL, ::fcntl(handle, F_GETFL) | O_NONBLOCK) == 0;
}
[[nodiscard]] bool set_io_timeout(SocketHandle handle, std::chrono::seconds timeout) {
  suppress_sigpipe(handle);
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count());
  tv.tv_usec = 0;
  return ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
         ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}
[[nodiscard]] bool connect_in_progress(int) { return false; }
[[nodiscard, maybe_unused]] bool peek_saw_eof(SocketHandle handle) {
  char byte = 0;
  return ::recv(handle, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

#endif

// Fills a sockaddr_un for `path`. False if the path is too long for the
// platform's sun_path (a hard limit, ~104 bytes on macOS, 108 on Linux and
// Windows) -- a clear failure rather than a silently truncated endpoint.
[[nodiscard]] bool fill_sockaddr(sockaddr_un& addr, const std::filesystem::path& path) {
  const std::string bytes = path.string();
  if (bytes.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, bytes.c_str(), bytes.size() + 1);
  return true;
}

// Waits until `handle` is ready for `events` or `deadline` passes; true when
// ready. No deadline: always ready (the blocking call itself waits).
[[nodiscard]] bool ready_before(SocketHandle handle, short events, const SocketDeadline& deadline) {
  if (!deadline) return true;
  while (true) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return false;
    PollEntry entry{};
    entry.fd = handle;
    entry.events = events;
    const int got = poll_entries(&entry, 1, static_cast<int>(left.count()));
    if (got > 0) return true;
    if (got == 0 || !interrupted()) return false;
  }
}

}  // namespace

LocalSocket& LocalSocket::operator=(LocalSocket&& other) noexcept {
  if (this != &other) {
    if (valid()) {
      close_handle(handle_);
    }
    handle_ = other.release();
  }
  return *this;
}

LocalSocket::~LocalSocket() {
  if (valid()) {
    close_handle(handle_);
  }
}

SocketHandle LocalSocket::release() {
  const auto handle = handle_;
  handle_ = kInvalidSocket;
  return handle;
}

std::string last_socket_error() {
  return std::system_category().message(last_error_code());
}

std::size_t max_socket_path_bytes() {
  return sizeof(sockaddr_un{}.sun_path) - 1;
}

LocalSocket listen_on_local_socket(const std::filesystem::path& path, std::string& error) {
  sockaddr_un addr{};
  if (!fill_sockaddr(addr, path)) {
    error = "socket path too long: " + path.string();
    return {};
  }
  LocalSocket listener(new_stream_socket());
  if (!listener.valid()) {
    error = "socket() failed: " + last_socket_error();
    return {};
  }
  if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "bind() failed: " + last_socket_error();
    return {};
  }
  if (::listen(listener.get(), 16) != 0) {
    error = "listen() failed: " + last_socket_error();
    return {};
  }
  return listener;
}

LocalSocket connect_local_socket(const std::filesystem::path& path, const SocketDeadline& deadline) {
  if (deadline && std::chrono::steady_clock::now() >= *deadline) {
    return {};
  }
  sockaddr_un addr{};
  if (!fill_sockaddr(addr, path)) {
    return {};
  }
  LocalSocket socket(new_stream_socket());
  if (!socket.valid()) {
    return {};
  }
  // With a deadline nothing blocks: a connect the listener's full backlog
  // would park fails (or, on Windows, completes within the deadline), and
  // every read and write waits in poll() for what is left of the deadline.
  if (deadline && !set_non_blocking(socket.get())) {
    return {};
  }
  if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    return socket;
  }
  if (!deadline || !connect_in_progress(last_error_code()) || !ready_before(socket.get(), kWritable, deadline)) {
    return {};  // refused / absent / stale -> the caller spawns and retries
  }
  int code = 0;
  auto length = static_cast<SockLen>(sizeof(code));
  if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) != 0 || code != 0) {
    return {};
  }
  return socket;
}

SocketWait wait_for_connection(const LocalSocket& listener, std::chrono::milliseconds timeout) {
  PollEntry entry{};
  entry.fd = listener.get();
  entry.events = kReadable;
  const int got = poll_entries(&entry, 1, static_cast<int>(timeout.count()));
  if (got > 0) return SocketWait::Ready;
  if (got == 0 || interrupted()) return SocketWait::TimedOut;
  return SocketWait::Failed;
}

LocalSocket accept_local_connection(const LocalSocket& listener, std::chrono::seconds io_timeout) {
  LocalSocket connection;
  do {
    connection = LocalSocket(static_cast<SocketHandle>(::accept(listener.get(), nullptr, nullptr)));
  } while (!connection.valid() && interrupted());
  // Best-effort: without the timeout this one connection can still block the
  // serve loop, which is logged; serving goes on.
  if (connection.valid() && !set_io_timeout(connection.get(), io_timeout)) {
    std::cerr << "graphd: setsockopt(timeout) failed: " << last_socket_error() << '\n';
  }
  return connection;
}

bool send_all(const LocalSocket& socket, const std::uint8_t* data, std::size_t size, const SocketDeadline& deadline) {
  std::size_t sent = 0;
  while (sent < size) {
    if (!ready_before(socket.get(), kWritable, deadline)) return false;
    const long written = send_some(socket.get(), data + sent, size - sent);
    if (written < 0) {
      if (interrupted()) continue;
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

bool receive_exact(const LocalSocket& socket, std::uint8_t* data, std::size_t size, const SocketDeadline& deadline) {
  std::size_t received = 0;
  while (received < size) {
    if (!ready_before(socket.get(), kReadable, deadline)) return false;
    const long got = receive_some(socket.get(), data + received, size - received);
    if (got == 0) return false;  // peer closed early
    if (got < 0) {
      if (interrupted()) continue;
      return false;
    }
    received += static_cast<std::size_t>(got);
  }
  return true;
}

bool peer_hung_up(const LocalSocket& socket) {
  PollEntry entry{};
  entry.fd = socket.get();
#if defined(POLLRDHUP) && !defined(_WIN32)
  entry.events = POLLIN | POLLRDHUP;
  return poll_entries(&entry, 1, 0) > 0 && (entry.revents & (POLLHUP | POLLRDHUP | POLLERR)) != 0;
#else
  entry.events = kReadable;
  if (poll_entries(&entry, 1, 0) <= 0) return false;
  if ((entry.revents & (POLLHUP | POLLERR)) != 0) return true;
  // Readable with nothing to read is an orderly close; the peek cannot block
  // because poll just reported the socket readable.
  return (entry.revents & kReadable) != 0 && peek_saw_eof(socket.get());
#endif
}

bool remove_socket_file(const std::filesystem::path& path) {
#ifdef _WIN32
  if (::DeleteFileW(path.c_str())) {
    return true;
  }
  const DWORD code = ::GetLastError();
  return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
#else
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
#endif
}

}  // namespace cgraph
