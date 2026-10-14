#pragma once

#include "cgraph/detect.hpp"
#include "cgraph/extractor.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace cgraph {

// `project_root` must be the canonical root the file was detected under: node
// ids derive from the file's path relative to it. A file outside the root is
// an extraction failure (reported as a warning, no nodes), never an id that
// climbs above the root.
[[nodiscard]] ExtractionResult extract_detected_file(const DetectedFile& file,
                                                     const std::filesystem::path& project_root);

// The per-language extractor `extract_detected_file` runs on a file's source.
// It defaults to `extract_configured_language`; the overload below takes one so
// a test can make an extractor throw.
using LanguageExtractor =
    std::function<std::optional<ExtractionResult>(DetectedLanguage, const ExtractionContext&)>;

// A failed extraction (any exception, including the extractor's) returns no
// nodes, a warning, and an EMPTY source_sha256: the hash marks content as
// already extracted, so it is only recorded once extraction has completed.
[[nodiscard]] ExtractionResult extract_detected_file(const DetectedFile& file,
                                                     const std::filesystem::path& project_root,
                                                     const LanguageExtractor& extractor);

// Extracts every file concurrently across a bounded worker pool and returns the
// results in input order. The sequence is identical to calling
// extract_detected_file on each file serially; only the wall time differs.
[[nodiscard]] std::vector<ExtractionResult> extract_files(std::span<const DetectedFile> files,
                                                          const std::filesystem::path& project_root);

}  // namespace cgraph
