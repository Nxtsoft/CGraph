#pragma once

#include "cgraph/daemon_identity.hpp"

#include <filesystem>

namespace cgraph {

// The per-user directory holding daemon sockets: $XDG_RUNTIME_DIR/graphd on
// POSIX, %LOCALAPPDATA%\graphd on Windows, else a per-user temp directory.
[[nodiscard]] std::filesystem::path unix_runtime_dir();
// The AF_UNIX socket path for one project's daemon (both platforms).
[[nodiscard]] std::filesystem::path unix_socket_path(const DaemonIdentity& identity);
// True if a live daemon is currently listening on socket_path. A stale socket
// file left by a crashed daemon refuses the connection (returns false); a live
// listener accepts it (returns true).
[[nodiscard]] bool unix_endpoint_is_live(const std::filesystem::path& socket_path);
// Creates the socket's directory and restricts it to the current user.
void ensure_unix_socket_dir(const std::filesystem::path& socket_path);

}  // namespace cgraph
