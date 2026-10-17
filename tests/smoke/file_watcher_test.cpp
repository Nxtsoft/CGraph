#include "cgraph/file_watcher.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
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
  // The debounce window runs from the FIRST write (seen at +70ms), so the event
  // is due at +120ms and the second write does not push it out. It carries the
  // latest state, and nothing is left pending afterwards.
  const auto modified = watcher.poll(start + 130ms);
  if (modified.size() != 1 ||
      !has_event(modified, code, cgraph::FileWatchChange::Modified, cgraph::WatchedFileKind::Code)) {
    return 1;
  }
  if (!watcher.poll(start + 160ms).empty()) {
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

  // === Debounce anchors to the first change (CGR-19) ===
  //
  // A file that changes on every poll must still be reported once the debounce
  // window since its FIRST change has elapsed, not starve because each new
  // change restarts the wait.
  {
    const auto busy_root = std::filesystem::temp_directory_path() / "cgraph-watcher-busy-test";
    std::filesystem::remove_all(busy_root);
    std::filesystem::create_directories(busy_root);
    const auto busy = busy_root / "src" / "busy.py";
    write_file(busy, "x = 0\n");

    cgraph::FileWatcher busy_watcher(busy_root, cgraph::FileWatcherOptions{.debounce = 50ms});
    const auto b0 = cgraph::FileWatcherClock::time_point{} + 2000s;
    (void)busy_watcher.poll(b0);  // prime

    // Each poll sees a new size, so a Modified is queued every 20ms. The first
    // change is seen at +20ms; the 50ms window ends by +80ms, not never.
    std::vector<std::chrono::milliseconds> emitted_at;
    std::string contents = "x = 0\n";
    for (int step = 1; step <= 8; ++step) {
      contents += "x = " + std::to_string(step) + "\n";
      write_file(busy, contents);
      const auto at = std::chrono::milliseconds(step * 20);
      const auto events = busy_watcher.poll(b0 + at);
      if (!events.empty()) {
        if (events.size() != 1 ||
            !has_event(events, busy, cgraph::FileWatchChange::Modified, cgraph::WatchedFileKind::Code)) {
          return 2;
        }
        emitted_at.push_back(at);
      }
    }
    // First flush at +80ms (change at +20ms, 60ms elapsed >= 50ms; at +60ms only
    // 40ms had). The next change starts a fresh window at +100ms, flushed at +160ms.
    if (emitted_at.size() != 2 || emitted_at[0] != 80ms || emitted_at[1] != 160ms) {
      return 3;
    }
    std::filesystem::remove_all(busy_root);
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
