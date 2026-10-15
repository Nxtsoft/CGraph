#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace cgraph {

// What a poll compares to decide a file changed. The change time catches a
// rewrite whose mtime was restored; size and mtime catch a rewrite that lands
// in the same change-time tick (timestamps advance in ticks: a few ms on
// Linux, coarser on NTFS).
struct FileChangeToken {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::int64_t ctime_sec = 0;
  long ctime_nsec = 0;
  std::uint64_t size = 0;
  std::int64_t mtime_sec = 0;
  long mtime_nsec = 0;

  bool operator==(const FileChangeToken&) const = default;
};

using FileWatcherClock = std::chrono::steady_clock;

enum class WatchedFileKind {
  Code,
  Document,
  Media,
};

enum class FileWatchChange {
  Created,
  Modified,
  Deleted,
  Overflow,
};

struct FileWatchEvent {
  std::filesystem::path path;
  FileWatchChange change = FileWatchChange::Modified;
  WatchedFileKind kind = WatchedFileKind::Code;
};

struct FileWatcherOptions {
  std::chrono::milliseconds debounce{250};
  std::size_t max_pending_events = 0;
};

class FileWatcher {
 public:
  struct FileState {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified_at{};
    WatchedFileKind kind = WatchedFileKind::Code;
    FileChangeToken token{};
  };

  FileWatcher(std::filesystem::path root, FileWatcherOptions options = {});

  [[nodiscard]] std::vector<FileWatchEvent> poll(FileWatcherClock::time_point now);

 private:
  struct PendingEvent {
    FileWatchEvent event;
    FileWatcherClock::time_point first_seen{};
  };

  std::filesystem::path root_;
  FileWatcherOptions options_;
  bool initialized_ = false;
  std::unordered_map<std::string, FileState> known_;
  std::unordered_map<std::string, PendingEvent> pending_;
};

[[nodiscard]] FileChangeToken read_file_change_token(const std::filesystem::path& path);
[[nodiscard]] WatchedFileKind classify_watched_file(const std::filesystem::path& path);
[[nodiscard]] bool is_watchable_file(const std::filesystem::path& path);

}  // namespace cgraph
