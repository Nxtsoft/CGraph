#include "cgraph/platform.hpp"

#include <cstdlib>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <cstdio>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace cgraph {

std::optional<std::string> environment_value(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

std::string executable_file_name(std::string_view stem) {
#ifdef _WIN32
  return std::string(stem) + ".exe";
#else
  return std::string(stem);
#endif
}

std::filesystem::path home_directory() {
  if (auto home = environment_value("HOME")) {
    return *home;
  }
#ifdef _WIN32
  if (auto profile = environment_value("USERPROFILE")) {
    return *profile;
  }
#endif
  return {};
}

std::string current_user_name() {
#ifdef _WIN32
  return environment_value("USERNAME").value_or(std::string{});
#else
  return environment_value("USER").value_or(std::string{});
#endif
}

std::wstring quote_windows_argument(std::wstring_view argument) {
  // CommandLineToArgvW rules: an argument needs quotes when empty or when it
  // holds whitespace or a quote. Inside quotes, backslashes are literal unless
  // they precede a quote (or the closing quote), where each must be doubled.
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t ch : argument) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

#ifdef _WIN32

std::uint64_t current_process_id() {
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

std::filesystem::path current_executable_path() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return {};
    }
    if (size < buffer.size()) {
      buffer.resize(size);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);  // truncated: retry with room
  }
}

bool restrict_directory_to_owner(const std::filesystem::path& directory) {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  DWORD needed = 0;
  (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  std::vector<unsigned char> buffer(needed);
  const bool got_user = needed > 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
  ::CloseHandle(token);
  if (!got_user) {
    return false;
  }
  PSID user_sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;

  // One ACE: full control for the owner, inherited by every file and
  // directory created inside (the socket file included). PROTECTED drops the
  // ACEs the profile directory would otherwise pass down.
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(user_sid);
  PACL acl = nullptr;
  if (::SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
    return false;
  }
  std::wstring name = directory.wstring();
  const DWORD result = ::SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, acl, nullptr);
  ::LocalFree(acl);
  return result == ERROR_SUCCESS;
}

bool spawn_detached(const std::filesystem::path& executable, const std::vector<std::filesystem::path>& arguments) {
  std::wstring command_line = quote_windows_argument(executable.wstring());
  for (const auto& argument : arguments) {
    command_line += L' ';
    command_line += quote_windows_argument(argument.wstring());
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // No inherited handles: the caller's pipes never stay open in the daemon.
  // DETACHED_PROCESS: no console, so closing the caller's window does not
  // take the daemon with it; a new process group keeps Ctrl+C in the caller's
  // console from reaching it.
  const std::wstring application = executable.wstring();
  if (!::CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        nullptr, &startup, &process)) {
    return false;
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return true;
}

void use_binary_stdio() {
  (void)::_setmode(::_fileno(stdin), _O_BINARY);
  (void)::_setmode(::_fileno(stdout), _O_BINARY);
}

#else  // POSIX

std::uint64_t current_process_id() {
  return static_cast<std::uint64_t>(::getpid());
}

std::filesystem::path current_executable_path() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  buffer.resize(std::strlen(buffer.c_str()));
  std::error_code ec;
  const auto canonical = std::filesystem::canonical(buffer, ec);
  return ec ? std::filesystem::path{buffer} : canonical;
#elif defined(__linux__)
  std::error_code ec;
  const auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : path;
#else
  return {};
#endif
}

bool restrict_directory_to_owner(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::permissions(
      directory,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace, error);
  return !error;
}

bool spawn_detached(const std::filesystem::path& executable, const std::vector<std::filesystem::path>& arguments) {
  // Everything the child needs is built before fork: only async-signal-safe
  // calls run between fork and exec.
  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 1);
  storage.push_back(executable.string());
  for (const auto& argument : arguments) {
    storage.push_back(argument.string());
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (auto& value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);

  const auto pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    // A new session (no controlling terminal) and stdio pointed at
    // /dev/null, so the resident child never holds the caller's pipes open
    // (which would make the caller's caller hang waiting for EOF).
    ::setsid();
    if (const int devnull = ::open("/dev/null", O_RDWR); devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        ::close(devnull);
      }
    }
    ::execv(argv[0], argv.data());
    std::_Exit(127);
  }
  return true;
}

void use_binary_stdio() {}

#endif

}  // namespace cgraph
