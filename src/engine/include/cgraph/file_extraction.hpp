#pragma once

#include "cgraph/detect.hpp"
#include "cgraph/extractor.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace cgraph {

// `project_root` must be the canonical root the file was detected under: node
// ids derive from the file's path relative to it. A file outside the root is
// an extraction failure (reported as a warning, no nodes), never an id that
// climbs above the root.
[[nodiscard]] ExtractionResult extract_detected_file(const DetectedFile& file,
                                                     const std::filesystem::path& project_root);

// Extracts every file concurrently across a bounded worker pool and returns the
// results in input order. The sequence is identical to calling
// extract_detected_file on each file serially; only the wall time differs.
[[nodiscard]] std::vector<ExtractionResult> extract_files(std::span<const DetectedFile> files,
                                                          const std::filesystem::path& project_root);

}  // namespace cgraph
