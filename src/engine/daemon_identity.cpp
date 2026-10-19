#include "cgraph/daemon_identity.hpp"

#include <iomanip>
#include <sstream>
#include <string>

namespace cgraph {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace

std::filesystem::path canonical_project_root(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    canonical = std::filesystem::absolute(path, error);
  }
  if (error) {
    canonical = path;
  }
  return canonical.lexically_normal();
}

std::string stable_project_hash(const std::filesystem::path& canonical_root) {
  // UTF-8 bytes on every platform (on POSIX these are the path's own bytes,
  // so existing hashes are unchanged; on Windows string() would go through
  // the ANSI code page and throw on characters it cannot map).
  const auto value = canonical_root.generic_u8string();
  std::uint64_t hash = kFnvOffset;
  for (const auto ch : value) {
#ifdef _WIN32
    // NTFS paths are case-insensitive: C:/Repo and c:/repo are one root and
    // must reach one daemon.
    const auto byte = static_cast<unsigned char>(ch >= u8'A' && ch <= u8'Z' ? ch - u8'A' + u8'a' : ch);
#else
    const auto byte = static_cast<unsigned char>(ch);
#endif
    hash ^= byte;
    hash *= kFnvPrime;
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << hash;
  return output.str();
}

DaemonIdentity daemon_identity_for(const std::filesystem::path& project_root) {
  auto canonical = canonical_project_root(project_root);
  auto hash = stable_project_hash(canonical);
  return DaemonIdentity{
      .project_root = std::move(canonical),
      .root_hash = hash,
      .endpoint_name = "graphd-" + hash,
  };
}

}  // namespace cgraph
