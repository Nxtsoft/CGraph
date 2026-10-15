#pragma once

// Process, environment and filesystem-permission seams. Each function has one
// POSIX and one Windows implementation in platform.cpp; callers stay free of
// platform conditionals.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// The calling process's id (getpid / GetCurrentProcessId).
[[nodiscard]] std::uint64_t current_process_id();

// Absolute path of the running executable, or empty when the platform cannot
// say (/proc/self/exe, _NSGetExecutablePath, GetModuleFileNameW).
[[nodiscard]] std::filesystem::path current_executable_path();

// `stem` with the platform's executable suffix: "graphd" -> "graphd.exe" on
// Windows, unchanged elsewhere.
[[nodiscard]] std::string executable_file_name(std::string_view stem);

// A non-empty environment variable, or nullopt.
[[nodiscard]] std::optional<std::string> environment_value(const char* name);

// The user's home directory: $HOME, or %USERPROFILE% on Windows. Empty when
// neither is set.
[[nodiscard]] std::filesystem::path home_directory();

// The login name: $USER, or %USERNAME% on Windows. Empty when unset.
[[nodiscard]] std::string current_user_name();

// Makes `directory` accessible to its owner only: mode 0700 on POSIX; on
// Windows a protected DACL granting full control to the current user's SID
// alone, inherited by everything created inside it. Returns false on failure.
[[nodiscard]] bool restrict_directory_to_owner(const std::filesystem::path& directory);

// Starts `executable` with `arguments`, detached from the caller: a new
// session (POSIX) or process group with no console (Windows), no inherited
// handles, and stdio pointed at the null device, so the child never holds the
// caller's pipes open and outlives it. Returns whether the process started.
[[nodiscard]] bool spawn_detached(const std::filesystem::path& executable,
                                  const std::vector<std::filesystem::path>& arguments);

// Quotes one argument for a Windows command line so CommandLineToArgvW (and
// the MSVC CRT) parse it back to exactly `argument`. Pure; used by
// spawn_detached on Windows and testable everywhere.
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

// Puts stdin and stdout in binary mode so newline-delimited protocols (MCP
// stdio JSON-RPC) see "\n", never "\r\n". No-op on POSIX.
void use_binary_stdio();

}  // namespace cgraph
