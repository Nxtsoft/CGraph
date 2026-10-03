#include "cgraph/atomic_write.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cgraph {
namespace {

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
  return path.parent_path() / (path.filename().string() + "." + std::to_string(::getpid()) + "." +
                               std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
}

// fsync() on macOS only hands the data to the drive's cache; F_FULLFSYNC asks
// the drive to flush it. Filesystems that do not implement F_FULLFSYNC (some
// network and FUSE mounts) reject it, and fsync() is the strongest sync they
// offer.
[[nodiscard]] int sync_to_disk(int fd) {
#if defined(F_FULLFSYNC)
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return 0;
  }
#endif
  return ::fsync(fd);
}

[[nodiscard]] int sync_directory(const std::filesystem::path& directory) {
  const auto target = directory.empty() ? std::filesystem::path(".") : directory;
  const int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return errno;
  }
  const int result = sync_to_disk(fd) == 0 ? 0 : errno;
  ::close(fd);
  return result;
}

}  // namespace

std::string try_write_file_atomically(const std::filesystem::path& path, std::string_view contents,
                                      WriteDurability durability) {
  remove_stale_temps(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(),
                     path.filename().string());

  const auto temp_path = unique_temp_path(path);
  const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (fd < 0) {
    return failure(path, "cannot create " + temp_path.string(), errno);
  }
  std::error_code ignored;
  const auto abandon = [&](std::string_view what, int error_number) {
    ::close(fd);
    std::filesystem::remove(temp_path, ignored);
    return failure(path, what, error_number);
  };

  const char* data = contents.data();
  std::size_t remaining = contents.size();
  while (remaining > 0) {
    const auto written = ::write(fd, data, remaining);
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

  struct stat status {};
  if (::fstat(fd, &status) != 0) {
    return abandon("cannot stat " + temp_path.string(), errno);
  }
  if (static_cast<std::uintmax_t>(status.st_size) != contents.size()) {
    return abandon("wrote " + std::to_string(status.st_size) + " of " + std::to_string(contents.size()) + " bytes", 0);
  }
  if (durability == WriteDurability::Durable && sync_to_disk(fd) != 0) {
    return abandon("cannot sync " + temp_path.string() + " to disk", errno);
  }
  if (::close(fd) != 0) {
    const int close_errno = errno;
    std::filesystem::remove(temp_path, ignored);
    return failure(path, "write did not complete", close_errno);
  }

  // rename() over an existing file is atomic on POSIX. On failure the prior file
  // at `path` is left exactly as it was; only the orphan temp is removed.
  std::error_code error;
  std::filesystem::rename(temp_path, path, error);
  if (error) {
    std::filesystem::remove(temp_path, ignored);
    return failure(path, "cannot rename " + temp_path.string() + " into place", error.value());
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
    throw FileWriteError(failure(directory, "cannot create directory", error.value()));
  }
}

}  // namespace cgraph
