#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cgraph {

// Raised by write_file_atomically. what() names the destination path and the
// OS reason (e.g. "No space left on device").
class FileWriteError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class WriteDurability {
  // Readers never see a partial file, but after a power loss the replacement
  // may not have reached the disk yet.
  Atomic,
  // Also syncs the file to stable storage before the rename (F_FULLFSYNC on
  // macOS) and the directory after it, so the replacement survives a power
  // loss. For files whose loss costs a rebuild: the daemon's graph.json and
  // index manifest.
  Durable,
};

// The one way the engine and CLI write an output file. `contents` goes to a
// temp file `<name>.<pid>.<n>.tmp` in the same directory (created exclusively,
// so concurrent writers -- threads or processes, e.g. the daemon and a CLI run
// writing the same cgraph-out file -- never share one); every write and the
// close must succeed and the temp file must hold exactly contents.size()
// bytes. Only then is it renamed over `path`, so a full disk never leaves a
// truncated file at `path`: a prior file there is left untouched and the temp
// file is removed. A crash between create and rename can still strand a temp
// file; each write therefore first removes this helper's temp files for the
// same `path` that are over an hour old.
//
// Replacing by rename means `path` becomes a new regular file: a symlink at
// `path` is replaced rather than written through, and the mode is the
// umask default rather than the prior file's.
//
// Returns an empty string on success, else a message naming `path`.
[[nodiscard]] std::string try_write_file_atomically(const std::filesystem::path& path, std::string_view contents,
                                                    WriteDurability durability = WriteDurability::Atomic);

// try_write_file_atomically, throwing FileWriteError on failure.
void write_file_atomically(const std::filesystem::path& path, std::string_view contents,
                           WriteDurability durability = WriteDurability::Atomic);

}  // namespace cgraph
