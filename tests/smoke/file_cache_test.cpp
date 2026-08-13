#include "cgraph/file_cache.hpp"
#include "cgraph/content_root.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define CGRAPH_FILE_CACHE_TEST_ASAN 1
#elif defined(__clang__)
#if __has_feature(address_sanitizer)
#define CGRAPH_FILE_CACHE_TEST_ASAN 1
#endif
#endif
#ifndef CGRAPH_FILE_CACHE_TEST_ASAN
#define CGRAPH_FILE_CACHE_TEST_ASAN 0
#endif

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

#if defined(__APPLE__) || defined(__unix__)
[[nodiscard]] std::uint64_t max_resident_bytes(const rusage& usage) {
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
}
#endif

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph-file-cache-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto path = root / "src" / "main.py";

  if (cgraph::sha256_hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
    return 1;
  }
  if (cgraph::sha256_hex(
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") !=
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") {
    return 1;
  }

  const auto empty_path = root / "empty.bin";
  write_file(empty_path, {});
  if (cgraph::sha256_file_hex(empty_path) !=
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    return 1;
  }

  const auto missing_path = root / "missing.bin";
  if (!cgraph::sha256_file_hex(missing_path).empty()) {
    return 1;
  }

  const auto directory_path = root / "directory";
  std::filesystem::create_directories(directory_path);
  if (!cgraph::sha256_file_hex(directory_path).empty()) {
    return 1;
  }

#if defined(__APPLE__) || defined(__unix__)
  const auto unreadable_path = root / "src" / "unreadable.py";
  write_file(unreadable_path, "class Unreadable:\n    pass\n");
  std::error_code permission_error;
  std::filesystem::permissions(
      unreadable_path,
      std::filesystem::perms::none,
      std::filesystem::perm_options::replace,
      permission_error);
  if (permission_error) {
    return 1;
  }
  const auto failed_digest = cgraph::sha256_file_hex(unreadable_path);
  const auto unreadable = cgraph::classify_cached_file(
      unreadable_path, std::nullopt, cgraph::CacheValidation::Content);
  std::filesystem::permissions(
      unreadable_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      permission_error);
  if (permission_error || !failed_digest.empty() || !unreadable.hash_computed ||
      unreadable.current.has_value()) {
    return 1;
  }
#endif

  // A million-plus-one repeated bytes crosses both SHA-256 block boundaries
  // and file-reader chunks, then finalizes a partial block while streaming.
  const auto streaming_path = root / "million-a.bin";
  write_file(streaming_path, std::string(1'000'001, 'a'));
  if (cgraph::sha256_file_hex(streaming_path) !=
      "9710f0882e9694259bf237c37b53b170f63b30b2addce6d498107ab6e4f9c3a5") {
    return 1;
  }

#if (defined(__APPLE__) || defined(__unix__)) && !CGRAPH_FILE_CACHE_TEST_ASAN
  // Hashing a 32 MiB real file may add only a bounded working set. Comparing
  // forked-child peaks subtracts the already-loaded test process and catches
  // implementations that retain/copy the complete input.
  const auto fixed_memory_path = root / "fixed-memory.bin";
  {
    std::ofstream output(fixed_memory_path, std::ios::binary);
    const std::array<char, 64U * 1024U> zeros{};
    for (std::size_t index = 0; index < 512U; ++index) {
      output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
  }

  rusage baseline_usage{};
  int baseline_status = 0;
  const auto baseline_child = ::fork();
  if (baseline_child == 0) {
    ::_exit(0);
  }
  if (baseline_child < 0 ||
      ::wait4(baseline_child, &baseline_status, 0, &baseline_usage) < 0 ||
      !WIFEXITED(baseline_status) || WEXITSTATUS(baseline_status) != 0) {
    return 1;
  }

  rusage hash_usage{};
  int hash_status = 0;
  const auto hash_child = ::fork();
  if (hash_child == 0) {
    const auto digest = cgraph::sha256_file_hex(fixed_memory_path);
    ::_exit(
        digest == "83ee47245398adee79bd9c0a8bc57b821e92aba10f5f9ade8a5d1fae4d8c4302"
            ? 0
            : 1);
  }
  if (hash_child < 0 ||
      ::wait4(hash_child, &hash_status, 0, &hash_usage) < 0 ||
      !WIFEXITED(hash_status) || WEXITSTATUS(hash_status) != 0) {
    return 1;
  }

  constexpr std::uint64_t kMaximumHashWorkingSet = 8U * 1024U * 1024U;
  if (max_resident_bytes(hash_usage) >
      max_resident_bytes(baseline_usage) + kMaximumHashWorkingSet) {
    return 1;
  }
#endif

  write_file(path, "print('hello')\n");
  const auto fresh = cgraph::classify_cached_file(path, std::nullopt);
  if (fresh.state != cgraph::CacheState::New || !fresh.hash_computed || !fresh.current.has_value()) {
    return 1;
  }

  const auto stat_hit = cgraph::classify_cached_file(path, fresh.current);
  if (stat_hit.state != cgraph::CacheState::StatHit || stat_hit.hash_computed ||
      stat_hit.current->sha256 != fresh.current->sha256) {
    return 1;
  }

  auto stat_mismatch_same_content = *fresh.current;
  stat_mismatch_same_content.size += 1;
  const auto hash_hit = cgraph::classify_cached_file(path, stat_mismatch_same_content);
  if (hash_hit.state != cgraph::CacheState::HashHit || !hash_hit.hash_computed ||
      hash_hit.current->sha256 != fresh.current->sha256) {
    return 1;
  }

  write_file(path, "print('changed')\n");
  const auto stale = cgraph::classify_cached_file(path, fresh.current);
  if (stale.state != cgraph::CacheState::Stale || !stale.hash_computed ||
      stale.current->sha256 == fresh.current->sha256) {
    return 1;
  }

  std::filesystem::remove(path);
  const auto deleted = cgraph::classify_cached_file(path, stale.current);
  if (deleted.state != cgraph::CacheState::Deleted || deleted.hash_computed || deleted.current.has_value()) {
    return 1;
  }

  // === Content validation mode ===

  write_file(path, "print('hello')\n");
  const auto content_fresh = cgraph::classify_cached_file(path, std::nullopt, cgraph::CacheValidation::Content);
  if (content_fresh.state != cgraph::CacheState::New || !content_fresh.hash_computed ||
      !content_fresh.current.has_value()) {
    return 1;
  }

  // Content mode must NEVER return StatHit — it always hashes
  const auto content_same = cgraph::classify_cached_file(
      path, content_fresh.current, cgraph::CacheValidation::Content);
  if (content_same.state != cgraph::CacheState::HashHit || !content_same.hash_computed) {
    return 1;
  }

  // Preserved-mtime rewrite: same size, restored mtime, different content
  const auto original_mtime = std::filesystem::last_write_time(path);
  write_file(path, "print('world')\n");
  std::filesystem::last_write_time(path, original_mtime);

  // Metadata mode: fooled — returns StatHit
  const auto meta_fooled = cgraph::classify_cached_file(
      path, content_fresh.current, cgraph::CacheValidation::Metadata);
  if (meta_fooled.state != cgraph::CacheState::StatHit) {
    return 1;
  }

  // Content mode: detects the stale content
  const auto content_caught = cgraph::classify_cached_file(
      path, content_fresh.current, cgraph::CacheValidation::Content);
  if (content_caught.state != cgraph::CacheState::Stale || !content_caught.hash_computed) {
    return 1;
  }

  // === Merkle root: empty tree ===

  const auto empty_root = cgraph::compute_content_root(root, std::span<const cgraph::FileCacheEntry>{});
  if (empty_root.algorithm != cgraph::kContentRootAlgorithm || empty_root.leaf_count != 0 ||
      !cgraph::is_valid_content_root(empty_root)) {
    return 1;
  }

  // Empty root is stable (same value every time)
  const auto empty_root2 = cgraph::compute_content_root(root, std::span<const cgraph::FileCacheEntry>{});
  if (empty_root.sha256 != empty_root2.sha256 || empty_root.sha256.size() != 64) {
    return 1;
  }

  // === Merkle root: deterministic across input ordering ===

  const auto file_a = root / "a.py";
  const auto file_b = root / "b.py";
  const auto file_c = root / "c.py";
  write_file(file_a, "a = 1\n");
  write_file(file_b, "b = 2\n");
  write_file(file_c, "c = 3\n");
  const auto entry_a = cgraph::read_file_cache_entry(file_a);
  const auto entry_b = cgraph::read_file_cache_entry(file_b);
  const auto entry_c = cgraph::read_file_cache_entry(file_c);
  std::vector<cgraph::FileCacheEntry> abc = {entry_a, entry_b, entry_c};
  std::vector<cgraph::FileCacheEntry> cba = {entry_c, entry_b, entry_a};
  std::vector<cgraph::FileCacheEntry> bca = {entry_b, entry_c, entry_a};
  const auto root_abc = cgraph::compute_content_root(root, abc);
  const auto root_cba = cgraph::compute_content_root(root, cba);
  const auto root_bca = cgraph::compute_content_root(root, bca);
  if (root_abc.sha256 != root_cba.sha256 || root_abc.sha256 != root_bca.sha256) {
    return 1;
  }
  if (root_abc.leaf_count != 3) {
    return 1;
  }

  // === Merkle root: changes on content change ===

  write_file(file_a, "a = 9\n");
  const auto entry_a_changed = cgraph::read_file_cache_entry(file_a);
  std::vector<cgraph::FileCacheEntry> changed_content = {entry_a_changed, entry_b, entry_c};
  const auto root_changed = cgraph::compute_content_root(root, changed_content);
  if (root_changed.sha256 == root_abc.sha256) {
    return 1;
  }

  // === Merkle root: changes on path change (same bytes) ===

  write_file(file_a, "a = 1\n");
  const auto entry_a_restored = cgraph::read_file_cache_entry(file_a);
  const auto file_d = root / "d.py";
  write_file(file_d, "a = 1\n");
  const auto entry_d = cgraph::read_file_cache_entry(file_d);
  if (entry_a_restored.sha256 != entry_d.sha256) {
    return 1;
  }
  std::vector<cgraph::FileCacheEntry> with_a = {entry_a_restored};
  std::vector<cgraph::FileCacheEntry> with_d = {entry_d};
  const auto root_with_a = cgraph::compute_content_root(root, with_a);
  const auto root_with_d = cgraph::compute_content_root(root, with_d);
  if (root_with_a.sha256 == root_with_d.sha256) {
    return 1;
  }

  // === Merkle root: changes on add / delete ===

  std::vector<cgraph::FileCacheEntry> two = {entry_a_restored, entry_b};
  const auto root_two = cgraph::compute_content_root(root, two);
  std::vector<cgraph::FileCacheEntry> three = {entry_a_restored, entry_b, entry_c};
  const auto root_three = cgraph::compute_content_root(root, three);
  if (root_two.sha256 == root_three.sha256) {
    return 1;
  }
  if (root_two.leaf_count != 2 || root_three.leaf_count != 3) {
    return 1;
  }

  auto malformed_entry = entry_a_restored;
  malformed_entry.sha256 = "not-a-sha256";
  try {
    (void)cgraph::compute_content_root(root, std::span<const cgraph::FileCacheEntry>{&malformed_entry, 1});
    return 1;
  } catch (const std::invalid_argument&) {
  }

  auto outside_entry = entry_a_restored;
  outside_entry.path = root.parent_path() / "outside.py";
  try {
    (void)cgraph::compute_content_root(root, std::span<const cgraph::FileCacheEntry>{&outside_entry, 1});
    return 1;
  } catch (const std::invalid_argument&) {
  }

  std::filesystem::remove_all(root);
  return 0;
}
