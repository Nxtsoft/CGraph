#pragma once

#include "cgraph/types.hpp"

#include <tree_sitter/api.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// Function fingerprints for `report clones` (CGR-10).
//
// A body is reduced to a token stream that survives the edits a copy usually
// gets: every identifier becomes `ID`, every string/number/character literal
// becomes `LIT`, comments vanish, and keywords, operators and punctuation keep
// their text. Consecutive runs of kShingleSize tokens are hashed (64-bit
// FNV-1a over the joined tokens); winnowing then keeps, from every window of
// kWinnowWindow consecutive shingle hashes, the smallest one (rightmost on a
// tie), so two bodies that share a stretch of tokens share the same selected
// hashes wherever that stretch lies. The Jaccard index of two winnowed sets is
// the similarity `report clones` thresholds at 0.80, the definition fallow and
// SourcererCC converged on for "80% similar".
inline constexpr std::size_t kShingleSize = 5;
inline constexpr std::size_t kWinnowWindow = 4;

// The normalized token stream of `node`'s subtree, in source order.
[[nodiscard]] std::vector<std::string> normalized_tokens(TSNode node, std::string_view source);

// Winnowed shingle hashes plus the normalized token count for `node`'s subtree
// (a function's body; the whole function when the grammar has no body field).
// Deterministic: the same tokens always give the same fingerprint.
[[nodiscard]] FunctionFingerprint fingerprint_function(TSNode node, std::string_view source);

// |A ∩ B| / |A ∪ B| over the shingle sets; 0 when both are empty.
[[nodiscard]] double fingerprint_similarity(const FunctionFingerprint& a, const FunctionFingerprint& b);

}  // namespace cgraph
