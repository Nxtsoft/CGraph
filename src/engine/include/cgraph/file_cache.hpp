#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cgraph {

enum class CacheState {
  New,
  StatHit,
  HashHit,
  Stale,
  Deleted,
  Unreadable,  // exists but its bytes cannot be read/hashed; carries no entry
};

enum class CacheValidation {
  Metadata,
  Content,
};

struct FileCacheEntry {
  std::filesystem::path path;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified_at{};
  std::string sha256;
};

struct FileCacheClassification {
  CacheState state = CacheState::New;
  bool hash_computed = false;
  std::optional<FileCacheEntry> current;
};

[[nodiscard]] std::string sha256_hex(std::string_view value);
[[nodiscard]] std::string sha256_file_hex(const std::filesystem::path& path);
[[nodiscard]] FileCacheEntry read_file_cache_entry(const std::filesystem::path& path);
[[nodiscard]] FileCacheClassification classify_cached_file(
    const std::filesystem::path& path,
    std::optional<FileCacheEntry> previous,
    CacheValidation mode = CacheValidation::Metadata);

}  // namespace cgraph
