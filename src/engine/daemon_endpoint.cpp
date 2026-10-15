#include "cgraph/daemon_endpoint.hpp"

#include "cgraph/local_socket.hpp"
#include "cgraph/platform.hpp"

#include <stdexcept>

namespace cgraph {

std::filesystem::path unix_runtime_dir() {
#ifdef _WIN32
  // Windows has no XDG runtime dir. %LOCALAPPDATA% sits in the user profile;
  // ensure_unix_socket_dir additionally restricts graphd/ to the owner.
  if (const auto local = environment_value("LOCALAPPDATA")) {
    return std::filesystem::path(*local) / "graphd";
  }
#else
  if (const auto runtime = environment_value("XDG_RUNTIME_DIR")) {
    return std::filesystem::path(*runtime) / "graphd";
  }
#endif

  auto temp = std::filesystem::temp_directory_path();
  if (const auto user = current_user_name(); !user.empty()) {
    return temp / ("graphd-" + user);
  }
  return temp / "graphd";
}

std::filesystem::path unix_socket_path(const DaemonIdentity& identity) {
  return unix_runtime_dir() / (identity.endpoint_name + ".sock");
}

bool unix_endpoint_is_live(const std::filesystem::path& socket_path) {
  // A live listener accepts the connection; an absent path or a stale socket
  // file left by a crashed daemon refuses it. No std::filesystem status query
  // first: on Windows an AF_UNIX socket file is a reparse point it cannot read.
  return connect_local_socket(socket_path).valid();
}

void ensure_unix_socket_dir(const std::filesystem::path& socket_path) {
  const auto dir = socket_path.parent_path();
  std::filesystem::create_directories(dir);
  if (!restrict_directory_to_owner(dir)) {
    throw std::runtime_error("cannot restrict " + dir.string() + " to its owner");
  }
}

}  // namespace cgraph
