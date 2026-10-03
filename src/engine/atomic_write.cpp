#include "cgraph/atomic_write.hpp"

#include <cerrno>
#include <fstream>
#include <system_error>

namespace cgraph {
namespace {

[[nodiscard]] std::string failure(const std::filesystem::path& path, std::string_view what, int error_number) {
  std::string message = "failed to write " + path.string() + ": " + std::string(what);
  if (error_number != 0) {
    message += " (" + std::generic_category().message(error_number) + ")";
  }
  return message;
}

}  // namespace

std::string try_write_file_atomically(const std::filesystem::path& path, std::string_view contents) {
  const auto temp_path = path.parent_path() / (path.filename().string() + ".tmp");
  std::error_code ignored;
  {
    errno = 0;
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return failure(path, "cannot create " + temp_path.string(), errno);
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    const int write_errno = errno;
    const bool written = static_cast<bool>(output);
    output.close();
    const int close_errno = errno;
    if (!written || output.fail()) {
      std::filesystem::remove(temp_path, ignored);
      return failure(path, "write did not complete", written ? close_errno : write_errno);
    }
  }

  std::error_code error;
  const auto size = std::filesystem::file_size(temp_path, error);
  if (error || size != contents.size()) {
    std::filesystem::remove(temp_path, ignored);
    return failure(path,
                   "wrote " + (error ? std::string("an unknown number of") : std::to_string(size)) + " of " +
                       std::to_string(contents.size()) + " bytes",
                   error.value());
  }

  // rename() over an existing file is atomic on POSIX. On failure the prior file
  // at `path` is left exactly as it was; only the orphan temp is removed.
  std::filesystem::rename(temp_path, path, error);
  if (error) {
    std::filesystem::remove(temp_path, ignored);
    return failure(path, "cannot rename " + temp_path.string() + " into place", error.value());
  }
  return {};
}

void write_file_atomically(const std::filesystem::path& path, std::string_view contents) {
  if (auto error = try_write_file_atomically(path, contents); !error.empty()) {
    throw FileWriteError(error);
  }
}

}  // namespace cgraph
