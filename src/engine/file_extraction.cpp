#include "cgraph/file_extraction.hpp"

#include "cgraph/configured_extractors.hpp"
#include "cgraph/file_cache.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace cgraph {
namespace {

constexpr std::uintmax_t kMaxExtractionFileBytes = 8 * 1024 * 1024;

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  std::error_code file_size_error;
  const auto size = std::filesystem::file_size(path, file_size_error);
  if (!file_size_error && size > kMaxExtractionFileBytes) {
    throw std::runtime_error("file too large for extraction");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open file");
  }

  std::string contents;
  if (!file_size_error) {
    contents.reserve(static_cast<std::size_t>(size));
  }

  char buffer[64 * 1024];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    if (count <= 0) {
      break;
    }
    if (contents.size() + static_cast<std::size_t>(count) > kMaxExtractionFileBytes) {
      throw std::runtime_error("file too large for extraction");
    }
    contents.append(buffer, static_cast<std::size_t>(count));
  }

  return contents;
}

[[nodiscard]] std::string relative_to_root(const std::filesystem::path& path,
                                           const std::filesystem::path& project_root) {
  const auto relative = path.lexically_normal().lexically_relative(project_root.lexically_normal());
  if (relative.empty() || relative.begin()->string() == "..") {
    throw std::runtime_error("file is outside the project root " + project_root.generic_string());
  }
  return relative.generic_string();
}

}  // namespace

ExtractionResult extract_detected_file(const DetectedFile& file, const std::filesystem::path& project_root) {
  return extract_detected_file(file, project_root, extract_configured_language);
}

ExtractionResult extract_detected_file(const DetectedFile& file,
                                       const std::filesystem::path& project_root,
                                       const LanguageExtractor& extractor) {
  ExtractionResult empty;
  try {
    const auto relative_path = relative_to_root(file.path, project_root);
    const auto source = read_file(file.path);
    const auto source_sha256 = sha256_hex(source);
    auto result = extractor(
        file.language,
        ExtractionContext{
            .source_file = file.path.generic_string(),
            .relative_path = relative_path,
            .source = source,
        });
    if (result.has_value()) {
      result->source_sha256 = source_sha256;
      return *result;
    }

    // Nothing is registered for this language, so there is nothing to retry:
    // the empty fragment is the settled result for this content.
    empty.source_sha256 = source_sha256;
    empty.fragment.warnings.push_back("no extractor registered for detected language");
    return empty;
  } catch (const std::exception& error) {
    empty.fragment.warnings.push_back(std::string{"failed to extract "} + file.path.generic_string() + ": " + error.what());
  } catch (...) {
    empty.fragment.warnings.push_back(std::string{"failed to extract "} + file.path.generic_string() + ": unknown error");
  }

  // A failed extraction leaves source_sha256 empty. Incremental update treats a
  // recorded hash as "this content was already extracted", so a hash here would
  // turn one crash into a permanent zero-node entry that is never retried.
  return empty;
}

std::vector<ExtractionResult> extract_files(std::span<const DetectedFile> files,
                                            const std::filesystem::path& project_root) {
  std::vector<ExtractionResult> results(files.size());
  if (files.empty()) {
    return results;
  }

  // One worker per core, never more than there are files. Each thread pulls the
  // next unclaimed index and writes its own slot, so there is no write
  // contention and the output order matches the input regardless of timing.
  const unsigned hardware = std::thread::hardware_concurrency();
  const std::size_t worker_count =
      std::min<std::size_t>(hardware == 0 ? 1 : hardware, files.size());

  if (worker_count <= 1) {
    for (std::size_t i = 0; i < files.size(); ++i) {
      results[i] = extract_detected_file(files[i], project_root);
    }
    return results;
  }

  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    for (std::size_t i = next.fetch_add(1, std::memory_order_relaxed); i < files.size();
         i = next.fetch_add(1, std::memory_order_relaxed)) {
      results[i] = extract_detected_file(files[i], project_root);
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(worker_count - 1);
  for (std::size_t i = 0; i < worker_count - 1; ++i) {
    workers.emplace_back(worker);
  }
  worker();  // the calling thread participates too
  for (auto& thread : workers) {
    thread.join();
  }
  return results;
}

}  // namespace cgraph
