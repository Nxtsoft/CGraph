#include "cgraph/file_watcher.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

bool has_event(
    const std::vector<cgraph::FileWatchEvent>& events,
    const std::filesystem::path& path,
    cgraph::FileWatchChange change,
    cgraph::WatchedFileKind kind) {
  for (const auto& event : events) {
    if (event.path == path && event.change == change && event.kind == kind) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  const auto root = std::filesystem::temp_directory_path() / "cgraph-file-watcher-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  cgraph::FileWatcher watcher(root, cgraph::FileWatcherOptions{.debounce = 50ms});
  const auto start = cgraph::FileWatcherClock::time_point{} + 100s;
  if (!watcher.poll(start).empty()) {
    return 1;
  }

  const auto code = root / "src" / "main.py";
  const auto doc = root / "docs" / "README.md";
  const auto media = root / "media" / "diagram.png";
  const auto ignored = root / "build" / "generated.cpp";

  write_file(code, "print('hello')\n");
  write_file(doc, "# Notes\n");
  write_file(media, "png");
  write_file(ignored, "int generated;\n");

  if (!watcher.poll(start + 10ms).empty()) {
    return 1;
  }
  const auto created = watcher.poll(start + 60ms);
  if (created.size() != 3 ||
      !has_event(created, code, cgraph::FileWatchChange::Created, cgraph::WatchedFileKind::Code) ||
      !has_event(created, doc, cgraph::FileWatchChange::Created, cgraph::WatchedFileKind::Document) ||
      !has_event(created, media, cgraph::FileWatchChange::Created, cgraph::WatchedFileKind::Media)) {
    return 1;
  }

  write_file(code, "print('first write with a longer body')\n");
  const auto settling = watcher.poll(start + 70ms);
  if (!settling.empty()) {
    return 1;
  }
  write_file(code, "print('second write with a much longer body than before')\n");
  if (!watcher.poll(start + 100ms).empty()) {
    return 1;
  }
  if (!watcher.poll(start + 130ms).empty()) {
    return 1;
  }
  const auto modified = watcher.poll(start + 160ms);
  if (modified.size() != 1 ||
      !has_event(modified, code, cgraph::FileWatchChange::Modified, cgraph::WatchedFileKind::Code)) {
    return 1;
  }

  std::filesystem::remove(doc);
  const auto delete_settling = watcher.poll(start + 170ms);
  if (!delete_settling.empty()) {
    return 1;
  }
  const auto deleted = watcher.poll(start + 230ms);
  if (deleted.size() != 1 ||
      !has_event(deleted, doc, cgraph::FileWatchChange::Deleted, cgraph::WatchedFileKind::Document)) {
    return 1;
  }

  // The watcher honors the root .gitignore exactly like detect_project_files:
  // a gitignored file must never produce an event, or an incremental update
  // would add nodes a full rescan later drops.
  write_file(root / ".gitignore", "scratch\n");
  const auto gitignored = root / "scratch" / "temp.py";
  const auto watched_peer = root / "src" / "peer.py";
  write_file(gitignored, "print('ignored')\n");
  write_file(watched_peer, "print('watched')\n");
  (void)watcher.poll(start + 240ms);
  const auto after_ignore = watcher.poll(start + 300ms);
  if (!has_event(after_ignore, watched_peer, cgraph::FileWatchChange::Created, cgraph::WatchedFileKind::Code) ||
      has_event(after_ignore, gitignored, cgraph::FileWatchChange::Created, cgraph::WatchedFileKind::Code)) {
    return 1;
  }

  // A flood of events past max_pending_events collapses into one Overflow (the
  // caller's signal to do a full rescan instead of per-file updates).
  cgraph::FileWatcher tiny(root, cgraph::FileWatcherOptions{.debounce = 50ms, .max_pending_events = 2});
  (void)tiny.poll(start + 400ms);  // prime
  write_file(root / "src" / "bulk_a.py", "a = 1\n");
  write_file(root / "src" / "bulk_b.py", "b = 2\n");
  write_file(root / "src" / "bulk_c.py", "c = 3\n");
  const auto overflow = tiny.poll(start + 410ms);
  if (overflow.size() != 1 || overflow[0].change != cgraph::FileWatchChange::Overflow ||
      overflow[0].path != root) {
    return 1;
  }

  // === POSIX file change token: same-size rewrite with restored mtime ===

  const auto token_root = std::filesystem::temp_directory_path() / "cgraph-watcher-token-test";
  std::filesystem::remove_all(token_root);
  std::filesystem::create_directories(token_root);

  cgraph::FileWatcher token_watcher(token_root, cgraph::FileWatcherOptions{.debounce = 50ms});
  const auto t0 = cgraph::FileWatcherClock::time_point{} + 1000s;
  (void)token_watcher.poll(t0);

  const auto token_file = token_root / "src" / "target.py";
  write_file(token_file, "x = 1\n");
  (void)token_watcher.poll(t0 + 10ms);
  const auto token_created = token_watcher.poll(t0 + 70ms);
  if (token_created.size() != 1 ||
      !has_event(token_created, token_file, cgraph::FileWatchChange::Created, cgraph::WatchedFileKind::Code)) {
    return 1;
  }

  const auto original_mtime = std::filesystem::last_write_time(token_file);
  write_file(token_file, "x = 2\n");
  std::filesystem::last_write_time(token_file, original_mtime);

  (void)token_watcher.poll(t0 + 80ms);
  const auto token_modified = token_watcher.poll(t0 + 140ms);
  if (token_modified.size() != 1 ||
      !has_event(token_modified, token_file, cgraph::FileWatchChange::Modified, cgraph::WatchedFileKind::Code)) {
    return 1;
  }

  // read_file_change_token returns a valid token for existing files
  const auto token = cgraph::read_file_change_token(token_file);
  if (token.device == 0 || token.inode == 0) {
    return 1;
  }

  // read_file_change_token returns a zero token for nonexistent files
  const auto missing_token = cgraph::read_file_change_token(token_root / "nonexistent.py");
  if (missing_token.device != 0 || missing_token.inode != 0 ||
      missing_token.ctime_sec != 0 || missing_token.ctime_nsec != 0) {
    return 1;
  }

  std::filesystem::remove_all(token_root);
  std::filesystem::remove_all(root);
  return 0;
}
