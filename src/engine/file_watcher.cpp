#include "cgraph/file_watcher.hpp"

#include "cgraph/detect.hpp"
#include "cgraph/path_ignore.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_set>

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
  struct stat sb {};
  if (::stat(path.c_str(), &sb) != 0) {
    return {};
  }
  FileChangeToken token;
  token.device = static_cast<std::uint64_t>(sb.st_dev);
  token.inode = static_cast<std::uint64_t>(sb.st_ino);
#if defined(__APPLE__)
  token.ctime_sec = static_cast<std::int64_t>(sb.st_ctimespec.tv_sec);
  token.ctime_nsec = static_cast<long>(sb.st_ctimespec.tv_nsec);
#else
  token.ctime_sec = static_cast<std::int64_t>(sb.st_ctim.tv_sec);
  token.ctime_nsec = static_cast<long>(sb.st_ctim.tv_nsec);
#endif
  return token;
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

  // first_seen anchors to the FIRST unflushed change for a path: a later change
  // replaces the pending event but must not push its deadline out, or a file
  // that changes on every poll (a log, a generated file, one an agent is still
  // writing) would never leave the debounce window. Same rule as
  // mark_graph_dirty in daemon_lifecycle.cpp.
  const auto queue = [&](const std::string& key, FileWatchChange change, WatchedFileKind kind) {
    FileWatchEvent event{.path = std::filesystem::path(key), .change = change, .kind = kind};
    const auto [pending, inserted] = pending_.try_emplace(key, PendingEvent{.event = event, .first_seen = now});
    if (!inserted) {
      pending->second.event = std::move(event);
    }
  };

  for (const auto& [key, state] : current) {
    const auto previous = known_.find(key);
    if (previous == known_.end()) {
      queue(key, FileWatchChange::Created, state.kind);
      continue;
    }
    if (previous->second.size != state.size || previous->second.modified_at != state.modified_at ||
        previous->second.token != state.token) {
      queue(key, FileWatchChange::Modified, state.kind);
    }
  }

  for (const auto& [key, state] : known_) {
    if (current.contains(key)) {
      continue;
    }
    queue(key, FileWatchChange::Deleted, state.kind);
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
