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

// The one way the engine and CLI write an output file. `contents` goes to a
// sibling temp file (`<name>.tmp`, same directory so the rename never crosses a
// filesystem); the stream state is checked after write, flush and close, and the
// temp file's size must equal contents.size(). Only then is it renamed over
// `path`, so a full disk or any other write failure never leaves a truncated
// file at `path`: a prior file there is left untouched and the temp file is
// removed. Returns an empty string on success, else a message naming `path`.
[[nodiscard]] std::string try_write_file_atomically(const std::filesystem::path& path, std::string_view contents);

// try_write_file_atomically, throwing FileWriteError on failure.
void write_file_atomically(const std::filesystem::path& path, std::string_view contents);

}  // namespace cgraph
