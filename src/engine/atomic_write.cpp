#include "cgraph/atomic_write.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <system_error>

#include "cgraph/platform.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <thread>
#else
#include <unistd.h>
#endif

namespace cgraph {
namespace {

// The few file-descriptor calls below, spelled per platform. The MSVC CRT
// offers the same descriptor model under underscored names; _O_BINARY keeps
// the bytes exact and _O_NOINHERIT matches O_CLOEXEC.
#ifdef _WIN32
using FileStatus = struct ::_stat64;
[[nodiscard]] int open_new_file(const std::filesystem::path& path) {
  return ::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
}
[[nodiscard]] long write_some(int fd, const char* data, std::size_t size) {
  return ::_write(fd, data, static_cast<unsigned>(std::min<std::size_t>(size, 1U << 30U)));
}
[[nodiscard]] int file_status(int fd, FileStatus& status) { return ::_fstat64(fd, &status); }
[[nodiscard]] int close_file(int fd) { return ::_close(fd); }

// MoveFileExW refuses to replace a file while any process holds it open
// without FILE_SHARE_DELETE, which is how std::ifstream and most readers open
// it. Such a reader is momentary (graph.json read by a client, say), so the
// replace is retried until it lets go, within a bounded window.
constexpr auto kReplaceRetryWindow = std::chrono::seconds(10);

[[nodiscard]] std::error_code replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
  const auto deadline = std::chrono::steady_clock::now() + kReplaceRetryWindow;
  auto pause = std::chrono::milliseconds(1);
  for (;;) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      return {};
    }
    const DWORD code = ::GetLastError();
    // A directory in the way also reports ERROR_ACCESS_DENIED, and no wait
    // will clear it.
    const DWORD target = ::GetFileAttributesW(to.c_str());
    const bool held_open = code == ERROR_SHARING_VIOLATION ||
                           (code == ERROR_ACCESS_DENIED && target != INVALID_FILE_ATTRIBUTES &&
                            (target & FILE_ATTRIBUTE_DIRECTORY) == 0);
    if (!held_open || std::chrono::steady_clock::now() >= deadline) {
      return {static_cast<int>(code), std::system_category()};
    }
    std::this_thread::sleep_for(pause);
    pause = std::min(pause * 2, std::chrono::milliseconds(50));
  }
}
#else
using FileStatus = struct ::stat;
[[nodiscard]] int open_new_file(const std::filesystem::path& path) {
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
}
[[nodiscard]] long write_some(int fd, const char* data, std::size_t size) {
  return static_cast<long>(::write(fd, data, size));
}
[[nodiscard]] int file_status(int fd, FileStatus& status) { return ::fstat(fd, &status); }
[[nodiscard]] int close_file(int fd) { return ::close(fd); }
// rename() replaces atomically, even while readers hold `to` open.
[[nodiscard]] std::error_code replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  return error;
}
#endif

// A temp file this old belongs to a writer that crashed (writes take seconds),
// so the next write to the same path may remove it.
constexpr auto kStaleTempAge = std::chrono::hours(1);

[[nodiscard]] std::string failure(const std::filesystem::path& path, std::string_view what, int error_number) {
  std::string message = "failed to write " + path.string() + ": " + std::string(what);
  if (error_number != 0) {
    message += " (" + std::generic_category().message(error_number) + ")";
  }
  return message;
}

[[nodiscard]] bool all_digits(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// True for exactly the names unique_temp_path makes for `name`:
// `<name>.<digits>.<digits>.tmp`.
[[nodiscard]] bool is_temp_name_for(std::string_view candidate, std::string_view name) {
  constexpr std::string_view suffix = ".tmp";
  if (candidate.size() <= name.size() + 1 + suffix.size() || !candidate.starts_with(name) ||
      candidate[name.size()] != '.' || !candidate.ends_with(suffix)) {
    return false;
  }
  const auto middle = candidate.substr(name.size() + 1, candidate.size() - name.size() - 1 - suffix.size());
  const auto dot = middle.find('.');
  return dot != std::string_view::npos && all_digits(middle.substr(0, dot)) && all_digits(middle.substr(dot + 1));
}

void remove_stale_temps(const std::filesystem::path& directory, const std::string& name) {
  std::error_code error;
  const auto now = std::filesystem::file_time_type::clock::now();
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    std::error_code entry_error;
    if (!is_temp_name_for(it->path().filename().string(), name) || !it->is_regular_file(entry_error)) {
      continue;
    }
    const auto modified = it->last_write_time(entry_error);
    if (!entry_error && now - modified > kStaleTempAge) {
      std::filesystem::remove(it->path(), entry_error);
    }
  }
}

[[nodiscard]] std::filesystem::path unique_temp_path(const std::filesystem::path& path) {
  static std::atomic<std::uint64_t> counter{0};
  return path.parent_path() / (path.filename().string() + "." + std::to_string(current_process_id()) + "." +
                               std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
}

// fsync() on macOS only hands the data to the drive's cache; F_FULLFSYNC asks
// the drive to flush it. Filesystems that do not implement F_FULLFSYNC (some
// network and FUSE mounts) reject it, and fsync() is the strongest sync they
// offer.
// On Windows _commit() is FlushFileBuffers.
[[nodiscard]] int sync_to_disk(int fd) {
#if defined(_WIN32)
  return ::_commit(fd);
#else
#if defined(F_FULLFSYNC)
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return 0;
  }
#endif
  return ::fsync(fd);
#endif
}

// Windows has no directory fsync: a directory cannot be opened as a file
// descriptor, and NTFS commits the rename through its metadata journal.
[[nodiscard]] int sync_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  (void)directory;
  return 0;
#else
  const auto target = directory.empty() ? std::filesystem::path(".") : directory;
  const int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return errno;
  }
  const int result = sync_to_disk(fd) == 0 ? 0 : errno;
  ::close(fd);
  return result;
#endif
}

}  // namespace

std::string try_write_file_atomically(const std::filesystem::path& path, std::string_view contents,
                                      WriteDurability durability) {
  remove_stale_temps(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(),
                     path.filename().string());

  const auto temp_path = unique_temp_path(path);
  const int fd = open_new_file(temp_path);
  if (fd < 0) {
    return failure(path, "cannot create " + temp_path.string(), errno);
  }
  std::error_code ignored;
  const auto abandon = [&](std::string_view what, int error_number) {
    (void)close_file(fd);
    std::filesystem::remove(temp_path, ignored);
    return failure(path, what, error_number);
  };

  const char* data = contents.data();
  std::size_t remaining = contents.size();
  while (remaining > 0) {
    const auto written = write_some(fd, data, remaining);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return abandon("write did not complete after " + std::to_string(contents.size() - remaining) + " of " +
                         std::to_string(contents.size()) + " bytes",
                     written < 0 ? errno : 0);
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }

  FileStatus status{};
  if (file_status(fd, status) != 0) {
    return abandon("cannot stat " + temp_path.string(), errno);
  }
  if (static_cast<std::uintmax_t>(status.st_size) != contents.size()) {
    return abandon("wrote " + std::to_string(status.st_size) + " of " + std::to_string(contents.size()) + " bytes", 0);
  }
  if (durability == WriteDurability::Durable && sync_to_disk(fd) != 0) {
    return abandon("cannot sync " + temp_path.string() + " to disk", errno);
  }
  if (close_file(fd) != 0) {
    const int close_errno = errno;
    std::filesystem::remove(temp_path, ignored);
    return failure(path, "write did not complete", close_errno);
  }

  // Replacing by rename is atomic (rename() on POSIX, MoveFileExW with
  // MOVEFILE_REPLACE_EXISTING on Windows). On failure the prior file at
  // `path` is left exactly as it was; only the orphan temp is removed.
  if (const auto error = replace_file(temp_path, path)) {
    std::filesystem::remove(temp_path, ignored);
    return failure(path, "cannot rename " + temp_path.string() + " into place: " + error.message(), 0);
  }
  if (durability == WriteDurability::Durable) {
    if (const int sync_errno = sync_directory(path.parent_path()); sync_errno != 0) {
      return failure(path, "replaced, but cannot sync its directory to disk", sync_errno);
    }
  }
  return {};
}

void write_file_atomically(const std::filesystem::path& path, std::string_view contents, WriteDurability durability) {
  if (auto error = try_write_file_atomically(path, contents, durability); !error.empty()) {
    throw FileWriteError(error);
  }
}

void create_output_directories(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    // error.message(), not failure()'s errno lookup: on Windows the code is a
    // Win32 error in system_category, which generic_category would misname.
    throw FileWriteError(failure(directory, "cannot create directory (" + error.message() + ")", 0));
  }
}

}  // namespace cgraph
