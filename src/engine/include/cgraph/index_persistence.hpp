#pragma once

#include "cgraph/content_root.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/file_cache.hpp"

#include "cgraph/operation_stats.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cgraph {

// A version-stamped snapshot of the file set a persisted graph.json was built
// from. On daemon restart, if the version key still matches the running binary
// and a full content verification reproduces every leaf and the aggregate root,
// the graph can be served straight from disk without re-extracting anything
// (the Tier-1 fast path).
struct IndexManifest {
  std::string version_key;
  std::vector<FileCacheEntry> files;
  ContentRoot content_root;
  // The route and client-call tallies of the build the graph came from, so a
  // fast-loaded daemon's status can report them; absent in older manifests.
  std::optional<ContractResolution> route_resolution;
};

// Logic version of the persisted pipeline artifacts. A persisted cache is
// fast-loadable only when this matches; it is bumped by hand when a parity
// surface changes (extraction/fragment shape, ID normalization, dedup/merge, or
// graph.json output). Deliberately a constant rather than a hash of the binary,
// so re-signing or an unrelated recompile does not invalidate every project's
// cache. Stable across calls and across binaries built from the same logic.
[[nodiscard]] std::string index_version_key();

// Atomic write (temp + rename). Returns false if the file could not be written.
[[nodiscard]] bool write_index_manifest(const IndexManifest& manifest, const std::filesystem::path& path);

// Returns nullopt if the manifest is absent or unparseable (treated as "no
// usable cache", never a half-loaded manifest).
[[nodiscard]] std::optional<IndexManifest> read_index_manifest(const std::filesystem::path& path);

// True iff a content-verified read of the complete detected file set matches
// every manifest leaf and recomputes the persisted root. Does NOT check the
// version key — the caller compares that against index_version_key() first.
// An empty project_root fails closed because a project-relative root cannot be
// verified without it.
[[nodiscard]] bool tree_matches_manifest(
    const IndexManifest& manifest,
    std::span<const DetectedFile> detected,
    const std::filesystem::path& project_root);

}  // namespace cgraph
