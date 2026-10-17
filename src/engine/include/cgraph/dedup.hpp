#pragma once

#include "cgraph/types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cgraph {

struct DedupOptions {
  double entropy_floor = 2.5;
  double jaro_winkler_threshold = 0.92;
  double same_community_threshold = 0.88;
};

// The hash behind the MinHash signatures that pick fuzzy-merge candidates.
// It is libstdc++'s std::hash<std::string> (64-bit _Hash_bytes, a MurmurHash2
// variant, seed 0xc70f6907) written out, so the candidate bands, and so the
// merges, are identical on every platform and standard library: MSVC hashes
// strings with FNV-1a and libc++ with CityHash, which merged different nodes.
// Linux output is unchanged.
[[nodiscard]] std::uint64_t minhash_shingle_hash(std::string_view bytes);
[[nodiscard]] double shannon_entropy(std::string_view value);
[[nodiscard]] double jaro_winkler_similarity(std::string_view lhs, std::string_view rhs);
void semantic_dedup(GraphSnapshot& graph, const DedupOptions& options = {});
void semantic_dedup_neighborhood(
    GraphSnapshot& graph,
    std::span<const std::string> changed_source_files,
    const DedupOptions& options = {});

}  // namespace cgraph
