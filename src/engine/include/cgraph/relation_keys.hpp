#pragma once

// "Is this fact already among the file's relations?" for an extractor that
// writes one fact per key (env_contracts, dynamo_contracts). Scanning every
// relation the file has so far on each fact made a file with many facts
// quadratic; a file scope instead holds the keys of the relations seen so far,
// caught up with the ones appended since the last lookup (a file's relations
// are only ever appended while it is extracted), so each relation is read once
// per file.

#include "cgraph/extractor.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace cgraph {

// Held by a file scope: the keys of the first `synced` relations of the one
// vector it covers (the first it is asked about).
template <typename Key>
struct RelationKeys {
  const std::vector<RawRelation>* relations = nullptr;
  std::size_t synced = 0;
  std::set<Key> keys;
};

// Whether `relations` holds a relation whose key is `key`. `key_of` gives a
// relation's key, or nullopt for one the caller does not track. `held` is the
// file scope's keys, or null without a scope; then, as for any vector other
// than the one the scope covers, the relations are scanned. `reads` counts
// the relations read, a test hook.
template <typename Key, typename KeyOf>
[[nodiscard]] bool has_relation_key(RelationKeys<Key>* held, const std::vector<RawRelation>& relations,
                                    const Key& key, KeyOf&& key_of, std::size_t& reads) {
  if (held == nullptr || (held->relations != nullptr && held->relations != &relations)) {
    return std::ranges::any_of(relations, [&](const RawRelation& relation) {
      ++reads;
      const auto found = key_of(relation);
      return found && *found == key;
    });
  }
  held->relations = &relations;
  assert(held->synced <= relations.size());  // relations are only appended
  for (; held->synced < relations.size(); ++held->synced) {
    ++reads;
    if (auto found = key_of(relations[held->synced])) {
      held->keys.insert(std::move(*found));
    }
  }
  return held->keys.contains(key);
}

}  // namespace cgraph
