#pragma once

// Test helper: makes a file unreadable to every other open until released.
// POSIX drops its mode to 0 (restored to 0600). Windows has no equivalent
// mode bits (std::filesystem::permissions only toggles the read-only
// attribute), so the guard holds the file open sharing nothing: every other
// open fails with a sharing violation, which readers see as unreadable.

#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace cgraph_test {

class UnreadableFile {
 public:
  explicit UnreadableFile(std::filesystem::path path) : path_(std::move(path)) {
#ifdef _WIN32
    handle_ = ::CreateFileW(path_.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    active_ = handle_ != INVALID_HANDLE_VALUE;
#else
    active_ = ::chmod(path_.c_str(), 0) == 0;
#endif
  }
  UnreadableFile(const UnreadableFile&) = delete;
  UnreadableFile& operator=(const UnreadableFile&) = delete;
  ~UnreadableFile() { (void)release(); }

  [[nodiscard]] bool active() const { return active_; }

  // Restores access; true on success.
  bool release() {
    if (!active_) return false;
    active_ = false;
#ifdef _WIN32
    return ::CloseHandle(handle_) != 0;
#else
    return ::chmod(path_.c_str(), 0600) == 0;
#endif
  }

 private:
  std::filesystem::path path_;
  bool active_ = false;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

}  // namespace cgraph_test
