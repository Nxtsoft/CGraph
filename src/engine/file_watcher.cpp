#include "cgraph/file_watcher.hpp"

#include "cgraph/detect.hpp"
#include "cgraph/path_ignore.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>

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

namespace cgraph {
namespace {

[[nodiscard]] std::string lower_ascii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
  });
  return value;
}

[[nodiscard]] bool is_document_extension(std::string_view extension) {
  static const std::unordered_set<std::string_view> docs = {
      ".md",
      ".markdown",
      ".mdx",
      ".rst",
      ".txt",
      ".adoc",
      ".org",
      ".pdf",
      ".doc",
      ".docx",
  };
  return docs.contains(extension);
}

[[nodiscard]] bool is_media_extension(std::string_view extension) {
  static const std::unordered_set<std::string_view> media = {
      ".png",
      ".jpg",
      ".jpeg",
      ".gif",
      ".webp",
      ".svg",
      ".mp3",
      ".wav",
      ".m4a",
      ".mp4",
      ".mov",
      ".webm",
  };
  return media.contains(extension);
}

[[nodiscard]] std::string key_for(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

// Walk the watched tree with the same ignore rules as detect_project_files
// (shared skip-list + root .gitignore), so the watcher never emits an event for
// a file the deterministic pipeline would not index — otherwise an incremental
// update could add nodes a full rescan later drops.
[[nodiscard]] std::unordered_map<std::string, FileWatcher::FileState> scan_files(const std::filesystem::path& root) {
  std::unordered_map<std::string, FileWatcher::FileState> files;
  const auto gitignore_patterns = read_root_gitignore(root);
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root,
      std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::recursive_directory_iterator end;

  for (; !error && iterator != end; iterator.increment(error)) {
    const auto& entry = *iterator;
    if (entry.is_directory(error)) {
      if (is_dependency_directory(entry.path()) ||
          matches_simple_gitignore(root, entry.path(), gitignore_patterns)) {
        iterator.disable_recursion_pending();
      }
      continue;
    }
    if (!entry.is_regular_file(error) || !is_watchable_file(entry.path())) {
      continue;
    }
    if (matches_simple_gitignore(root, entry.path(), gitignore_patterns)) {
      continue;
    }

    FileWatcher::FileState state;
    state.size = entry.file_size(error);
    if (error) {
      error.clear();
      continue;
    }
    state.modified_at = entry.last_write_time(error);
    if (error) {
      error.clear();
      continue;
    }
    state.kind = classify_watched_file(entry.path());
    state.token = read_file_change_token(entry.path());
    files.emplace(key_for(entry.path()), state);
  }
  return files;
}

}  // namespace

FileChangeToken read_file_change_token(const std::filesystem::path& path) {
#ifdef _WIN32
  // The Windows analogue of (st_dev, st_ino, st_ctim): the volume serial
  // number, the NTFS file index, and the change time (which, like ctime,
  // moves on content and metadata changes).
  const HANDLE file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  BY_HANDLE_FILE_INFORMATION identity{};
  FILE_BASIC_INFO basic{};
  const bool ok = ::GetFileInformationByHandle(file, &identity) &&
                  ::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
  ::CloseHandle(file);
  if (!ok) {
    return {};
  }
  FileChangeToken token;
  token.device = identity.dwVolumeSerialNumber;
  token.inode = (static_cast<std::uint64_t>(identity.nFileIndexHigh) << 32U) | identity.nFileIndexLow;
  // File times count 100 ns ticks; split into seconds + nanoseconds.
  const auto split = [](LARGE_INTEGER time, std::int64_t& sec, long& nsec) {
    sec = static_cast<std::int64_t>(time.QuadPart / 10'000'000);
    nsec = static_cast<long>((time.QuadPart % 10'000'000) * 100);
  };
  split(basic.ChangeTime, token.ctime_sec, token.ctime_nsec);
  split(basic.LastWriteTime, token.mtime_sec, token.mtime_nsec);
  token.size = (static_cast<std::uint64_t>(identity.nFileSizeHigh) << 32U) | identity.nFileSizeLow;
  return token;
#else
  struct stat sb {};
  if (::stat(path.c_str(), &sb) != 0) {
    return {};
  }
  FileChangeToken token;
  token.device = static_cast<std::uint64_t>(sb.st_dev);
  token.inode = static_cast<std::uint64_t>(sb.st_ino);
  token.size = static_cast<std::uint64_t>(sb.st_size);
#if defined(__APPLE__)
  token.ctime_sec = static_cast<std::int64_t>(sb.st_ctimespec.tv_sec);
  token.ctime_nsec = static_cast<long>(sb.st_ctimespec.tv_nsec);
  token.mtime_sec = static_cast<std::int64_t>(sb.st_mtimespec.tv_sec);
  token.mtime_nsec = static_cast<long>(sb.st_mtimespec.tv_nsec);
#else
  token.ctime_sec = static_cast<std::int64_t>(sb.st_ctim.tv_sec);
  token.ctime_nsec = static_cast<long>(sb.st_ctim.tv_nsec);
  token.mtime_sec = static_cast<std::int64_t>(sb.st_mtim.tv_sec);
  token.mtime_nsec = static_cast<long>(sb.st_mtim.tv_nsec);
#endif
  return token;
#endif
}

FileWatcher::FileWatcher(std::filesystem::path root, FileWatcherOptions options)
    : root_(std::move(root)), options_(options) {}

std::vector<FileWatchEvent> FileWatcher::poll(FileWatcherClock::time_point now) {
  const auto current = scan_files(root_);
  if (!initialized_) {
    known_ = current;
    initialized_ = true;
    return {};
  }

  for (const auto& [key, state] : current) {
    const auto previous = known_.find(key);
    if (previous == known_.end()) {
      pending_[key] = PendingEvent{
          .event = FileWatchEvent{.path = std::filesystem::path(key), .change = FileWatchChange::Created, .kind = state.kind},
          .first_seen = now,
      };
      continue;
    }
    if (previous->second.size != state.size || previous->second.modified_at != state.modified_at ||
        previous->second.token != state.token) {
      pending_[key] = PendingEvent{
          .event = FileWatchEvent{.path = std::filesystem::path(key), .change = FileWatchChange::Modified, .kind = state.kind},
          .first_seen = now,
      };
    }
  }

  for (const auto& [key, state] : known_) {
    if (current.contains(key)) {
      continue;
    }
    pending_[key] = PendingEvent{
        .event = FileWatchEvent{.path = std::filesystem::path(key), .change = FileWatchChange::Deleted, .kind = state.kind},
        .first_seen = now,
    };
  }

  known_ = current;
  if (options_.max_pending_events > 0 && pending_.size() > options_.max_pending_events) {
    pending_.clear();
    return {FileWatchEvent{
        .path = root_,
        .change = FileWatchChange::Overflow,
        .kind = WatchedFileKind::Code,
    }};
  }

  std::vector<FileWatchEvent> ready;
  for (auto iterator = pending_.begin(); iterator != pending_.end();) {
    if (now - iterator->second.first_seen >= options_.debounce) {
      ready.push_back(std::move(iterator->second.event));
      iterator = pending_.erase(iterator);
    } else {
      ++iterator;
    }
  }

  std::ranges::sort(ready, [](const FileWatchEvent& lhs, const FileWatchEvent& rhs) {
    return lhs.path.generic_string() < rhs.path.generic_string();
  });
  return ready;
}

WatchedFileKind classify_watched_file(const std::filesystem::path& path) {
  if (detect_language(path) != DetectedLanguage::Unknown) {
    return WatchedFileKind::Code;
  }
  const auto extension = lower_ascii(path.extension().generic_string());
  if (is_media_extension(extension)) {
    return WatchedFileKind::Media;
  }
  return WatchedFileKind::Document;
}

bool is_watchable_file(const std::filesystem::path& path) {
  if (detect_language(path) != DetectedLanguage::Unknown) {
    return true;
  }
  const auto extension = lower_ascii(path.extension().generic_string());
  return is_document_extension(extension) || is_media_extension(extension);
}

}  // namespace cgraph
