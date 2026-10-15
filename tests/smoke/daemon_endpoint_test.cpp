#include "cgraph/daemon_endpoint.hpp"
#include "cgraph/local_socket.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <vector>
#endif

namespace {

int fail(const std::string& message) {
  std::cerr << "daemon_endpoint_test: " << message << '\n';
  return 1;
}

#ifdef _WIN32
// The directory's DACL must be protected (nothing inherited from the profile)
// and grant access to exactly one SID: the current user's.
bool only_owner_can_access(const std::filesystem::path& dir) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (::GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS) {
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  const bool is_protected = ::GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                            (control & SE_DACL_PROTECTED) != 0;

  HANDLE token = nullptr;
  (void)::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD needed = 0;
  (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  std::vector<unsigned char> buffer(needed);
  const bool got_user = ::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
  ::CloseHandle(token);

  bool owner_only = got_user && dacl != nullptr && dacl->AceCount >= 1;
  for (DWORD index = 0; owner_only && index < dacl->AceCount; ++index) {
    void* ace = nullptr;
    owner_only = ::GetAce(dacl, index, &ace) &&
                 static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                 ::EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart,
                            reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
  }
  ::LocalFree(descriptor);
  return is_protected && owner_only;
}
#else
bool only_owner_can_access(const std::filesystem::path& dir) {
  const auto group_or_other = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
  return (std::filesystem::status(dir).permissions() & group_or_other) == std::filesystem::perms::none;
}
#endif

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_daemon_endpoint_test";
  std::filesystem::create_directories(root);

  const auto identity = cgraph::daemon_identity_for(root);
  const auto socket = cgraph::unix_socket_path(identity);
  if (socket.extension() != ".sock" || socket.parent_path() != cgraph::unix_runtime_dir()) {
    return fail("socket path not under the runtime dir: " + socket.string());
  }
  if (socket.string().size() > cgraph::max_socket_path_bytes()) {
    return fail("socket path exceeds sun_path: " + socket.string());
  }

  cgraph::ensure_unix_socket_dir(socket);
  if (!only_owner_can_access(socket.parent_path())) {
    return fail("runtime dir is reachable by other users: " + socket.parent_path().string());
  }

  // Liveness: nothing at the path, then a live listener, then the stale file
  // the listener leaves behind once closed.
  (void)cgraph::remove_socket_file(socket);
  if (cgraph::unix_endpoint_is_live(socket)) {
    return fail("absent endpoint reported live");
  }
  {
    std::string error;
    const auto listener = cgraph::listen_on_local_socket(socket, error);
    if (!listener.valid()) {
      return fail("listen failed: " + error);
    }
    if (!cgraph::unix_endpoint_is_live(socket)) {
      return fail("listening endpoint reported dead");
    }
  }
  if (cgraph::unix_endpoint_is_live(socket)) {
    return fail("stale socket file reported live");
  }
  if (!cgraph::remove_socket_file(socket)) {
    return fail("stale socket file not removed");
  }
  std::filesystem::remove_all(root);
  return 0;
}
