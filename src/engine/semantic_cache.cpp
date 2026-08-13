#include "cgraph/semantic_cache.hpp"

#include "cgraph/file_cache.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace cgraph {
namespace {

[[nodiscard]] std::string state_to_string(SemanticCacheState state) {
  switch (state) {
    case SemanticCacheState::Valid:
      return "valid";
    case SemanticCacheState::Stale:
      return "stale";
    case SemanticCacheState::Failed:
      return "failed";
  }
  return "failed";
}

[[nodiscard]] SemanticCacheState state_from_string(const std::string& state) {
  if (state == "valid") {
    return SemanticCacheState::Valid;
  }
  if (state == "stale") {
    return SemanticCacheState::Stale;
  }
  return SemanticCacheState::Failed;
}

[[nodiscard]] nlohmann::json dependency_to_json(const SemanticDependency& dep) {
  return {
      {"node_id", dep.node_id},
      {"source_path", dep.source_path},
      {"source_sha256", dep.source_sha256},
  };
}

[[nodiscard]] SemanticDependency dependency_from_json(const nlohmann::json& value) {
  return SemanticDependency{
      .node_id = value.at("node_id").get<std::string>(),
      .source_path = value.at("source_path").get<std::string>(),
      .source_sha256 = value.at("source_sha256").get<std::string>(),
  };
}

[[nodiscard]] nlohmann::json record_to_json(const SemanticCacheRecord& record) {
  auto deps = nlohmann::json::array();
  for (const auto& dep : record.dependencies) {
    deps.push_back(dependency_to_json(dep));
  }
  auto obj = nlohmann::json{
      {"source_path", record.source_path.generic_string()},
      {"content_hash", record.content_hash},
      {"fragment_path", record.fragment_path.generic_string()},
      {"fragment_hash", record.fragment_hash},
      {"state", state_to_string(record.state)},
      {"dependencies", std::move(deps)},
  };
  if (!record.last_error.empty()) {
    obj["last_error"] = record.last_error;
  }
  return obj;
}

[[nodiscard]] SemanticCacheRecord record_from_json(const nlohmann::json& value) {
  SemanticCacheRecord record;
  record.source_path = value.at("source_path").get<std::string>();
  record.content_hash = value.at("content_hash").get<std::string>();
  record.fragment_path = value.at("fragment_path").get<std::string>();
  record.fragment_hash = value.at("fragment_hash").get<std::string>();
  record.state = state_from_string(value.at("state").get<std::string>());
  if (value.contains("last_error")) {
    record.last_error = value.at("last_error").get<std::string>();
  }
  for (const auto& dependency : value.at("dependencies")) {
    record.dependencies.push_back(dependency_from_json(dependency));
  }
  return record;
}

[[nodiscard]] bool has_string_field(
    const nlohmann::json& value,
    const char* field,
    bool require_non_empty = false) {
  const auto item = value.find(field);
  return item != value.end() && item->is_string() &&
         (!require_non_empty || !item->get_ref<const std::string&>().empty());
}

[[nodiscard]] bool is_cache_record_json(const nlohmann::json& value) {
  if (!value.is_object()) {
    return false;
  }
  for (const auto* field : {"source_path", "content_hash", "fragment_path", "fragment_hash", "state"}) {
    if (!has_string_field(value, field, std::string_view{field} == "source_path")) {
      return false;
    }
  }
  const auto& state = value.at("state").get_ref<const std::string&>();
  if (state != "valid" && state != "stale" && state != "failed") {
    return false;
  }
  if (value.contains("last_error") && !value.at("last_error").is_string()) {
    return false;
  }
  if (!value.contains("dependencies") || !value["dependencies"].is_array()) {
    return false;
  }
  for (const auto& dependency : value["dependencies"]) {
    if (!dependency.is_object()) {
      return false;
    }
    for (const auto* field : {"node_id", "source_path", "source_sha256"}) {
      if (!has_string_field(dependency, field, true)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::string normalize_semantic_source_path(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  if (!path.is_absolute()) {
    return path.lexically_normal().generic_string();
  }
  std::error_code canonical_error;
  const auto canonical = std::filesystem::weakly_canonical(path, canonical_error);
  return canonical_error ? path.lexically_normal().generic_string()
                         : canonical.generic_string();
}

void SemanticCache::upsert(SemanticCacheRecord record) {
  const auto key = normalize_semantic_source_path(record.source_path);
  record.source_path = std::filesystem::path{key};
  if (!record.fragment_path.empty()) {
    record.fragment_path = std::filesystem::path{
        normalize_semantic_source_path(record.fragment_path)};
  }
  for (auto& dependency : record.dependencies) {
    if (!dependency.source_path.empty()) {
      dependency.source_path = normalize_semantic_source_path(dependency.source_path);
    }
  }
  records_by_source_[key] = std::move(record);
}

void SemanticCache::remove(const std::filesystem::path& source_path) {
  records_by_source_.erase(normalize_semantic_source_path(source_path));
}

std::optional<SemanticCacheRecord> SemanticCache::find_for_source(const std::filesystem::path& path) const {
  const auto it = records_by_source_.find(normalize_semantic_source_path(path));
  if (it == records_by_source_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<SemanticCacheRecord> SemanticCache::find_for_source(
    const std::filesystem::path& path,
    std::string_view content_hash) const {
  const auto record = find_for_source(path);
  if (!record.has_value() || record->content_hash != content_hash) {
    return std::nullopt;
  }
  return record;
}

std::vector<SemanticCacheRecord> SemanticCache::find_for_fragment(const std::filesystem::path& fragment_path) const {
  std::vector<SemanticCacheRecord> result;
  const auto key = normalize_semantic_source_path(fragment_path);
  for (const auto& [_, record] : records_by_source_) {
    if (normalize_semantic_source_path(record.fragment_path) == key) {
      result.push_back(record);
    }
  }
  return result;
}

std::vector<SemanticCacheRecord> SemanticCache::records() const {
  std::vector<SemanticCacheRecord> values;
  values.reserve(records_by_source_.size());
  for (const auto& [_, record] : records_by_source_) {
    values.push_back(record);
  }
  std::ranges::sort(values, [](const SemanticCacheRecord& lhs, const SemanticCacheRecord& rhs) {
    return lhs.source_path.generic_string() < rhs.source_path.generic_string();
  });
  return values;
}

std::size_t SemanticCache::size() const {
  return records_by_source_.size();
}

std::size_t SemanticCache::count_valid() const {
  std::size_t count = 0;
  for (const auto& [_, record] : records_by_source_) {
    if (record.state == SemanticCacheState::Valid) {
      ++count;
    }
  }
  return count;
}

std::size_t SemanticCache::count_stale() const {
  std::size_t count = 0;
  for (const auto& [_, record] : records_by_source_) {
    if (record.state == SemanticCacheState::Stale) {
      ++count;
    }
  }
  return count;
}

std::size_t SemanticCache::count_failed() const {
  std::size_t count = 0;
  for (const auto& [_, record] : records_by_source_) {
    if (record.state == SemanticCacheState::Failed) {
      ++count;
    }
  }
  return count;
}

ReconciliationResult reconcile_semantic_cache(
    SemanticCache& cache,
    const GraphSnapshot& graph) {
  ReconciliationResult result;

  std::unordered_map<std::string, const Node*> graph_nodes;
  graph_nodes.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    graph_nodes[node.id] = &node;
  }

  struct PreparedRecord {
    SemanticCacheRecord record;
    std::string source_key;
    std::string fragment_key;
  };
  std::vector<PreparedRecord> records;
  for (auto record : cache.records()) {
    auto source_key = normalize_semantic_source_path(record.source_path);
    auto fragment_key = normalize_semantic_source_path(record.fragment_path);
    records.push_back(PreparedRecord{
        .record = std::move(record),
        .source_key = std::move(source_key),
        .fragment_key = std::move(fragment_key),
    });
  }

  struct FileFingerprint {
    bool exists = false;
    std::string sha256;
  };
  std::unordered_map<std::string, FileFingerprint> file_fingerprints;
  const auto fingerprint = [&](const std::filesystem::path& path,
                               const std::string& normalized_path)
      -> const FileFingerprint& {
    auto [item, inserted] = file_fingerprints.try_emplace(normalized_path);
    if (inserted) {
      std::error_code exists_error;
      item->second.exists =
          std::filesystem::exists(path, exists_error) && !exists_error;
      if (item->second.exists) {
        item->second.sha256 = sha256_file_hex(path);
      }
    }
    return item->second;
  };

  std::unordered_map<std::string, std::string> direct_reasons;
  std::unordered_map<std::string, std::string> invalid_fragments;

  for (const auto& prepared : records) {
    const auto& record = prepared.record;
    ++result.records_checked;

    if (record.state != SemanticCacheState::Valid) {
      if (!prepared.fragment_key.empty()) {
        invalid_fragments.try_emplace(
            prepared.fragment_key,
            record.last_error.empty() ? "a mapped source record is not valid" : record.last_error);
      }
      continue;
    }

    std::string reason;
    if (record.content_hash.empty()) {
      reason = "semantic source fingerprint is missing";
    } else {
      const auto& source = fingerprint(record.source_path, prepared.source_key);
      if (!source.exists) {
        reason = "semantic source file missing: " + prepared.source_key;
      } else if (source.sha256.empty()) {
        reason =
            "semantic source fingerprint read failed: " + prepared.source_key;
      } else if (source.sha256 != record.content_hash) {
        reason = "semantic source hash changed";
      } else if (record.fragment_hash.empty()) {
        reason = "fragment fingerprint is missing";
      } else {
        const auto& fragment =
            fingerprint(record.fragment_path, prepared.fragment_key);
        if (!fragment.exists) {
          reason = "fragment file missing: " + prepared.fragment_key;
        } else if (fragment.sha256.empty()) {
          reason = "fragment fingerprint read failed: " + prepared.fragment_key;
        } else if (fragment.sha256 != record.fragment_hash) {
          reason = "fragment hash changed";
        }
      }
    }

    if (reason.empty()) {
      for (const auto& dependency : record.dependencies) {
        if (dependency.node_id.empty() || dependency.source_path.empty() ||
            dependency.source_sha256.empty()) {
          reason = "dependency fingerprint is incomplete";
          break;
        }
        const auto node = graph_nodes.find(dependency.node_id);
        if (node == graph_nodes.end()) {
          reason = "dependency node missing: " + dependency.node_id;
          break;
        }
        const auto current_source = normalize_semantic_source_path(node->second->source_file);
        const auto expected_source = normalize_semantic_source_path(dependency.source_path);
        if (current_source != expected_source) {
          reason = "dependency source path changed for " + dependency.node_id;
          break;
        }
        const auto current_hash = graph.source_hashes.find(current_source);
        if (current_hash == graph.source_hashes.end()) {
          reason = "dependency source hash missing for " + dependency.node_id;
          break;
        }
        if (current_hash->second != dependency.source_sha256) {
          reason = "dependency source hash changed for " + dependency.node_id;
          break;
        }
      }
    }

    if (!reason.empty()) {
      direct_reasons.emplace(prepared.source_key, reason);
      if (!prepared.fragment_key.empty()) {
        invalid_fragments.try_emplace(prepared.fragment_key, reason);
      }
    }
  }

  for (auto& prepared : records) {
    auto& record = prepared.record;
    if (record.state != SemanticCacheState::Valid) {
      ++result.records_preserved;
      continue;
    }
    auto reason = direct_reasons.find(prepared.source_key);
    std::string invalid_reason;
    if (reason != direct_reasons.end()) {
      invalid_reason = reason->second;
    } else if (const auto peer = invalid_fragments.find(prepared.fragment_key);
               peer != invalid_fragments.end()) {
      invalid_reason = "mapped fragment is not reusable: " + peer->second;
    }

    if (invalid_reason.empty()) {
      ++result.records_preserved;
      continue;
    }
    record.state = SemanticCacheState::Stale;
    record.last_error = std::move(invalid_reason);
    cache.upsert(std::move(record));
    ++result.records_invalidated;
  }

  return result;
}

void write_semantic_cache(const SemanticCache& cache, const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  auto records = nlohmann::json::array();
  for (const auto& record : cache.records()) {
    records.push_back(record_to_json(record));
  }

  std::ofstream output(path, std::ios::binary);
  output << nlohmann::json{{"version", 2}, {"records", std::move(records)}}.dump(2);
}

SemanticCache read_semantic_cache(const std::filesystem::path& path) {
  SemanticCache cache;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return cache;
  }

  const auto root = nlohmann::json::parse(input, nullptr, false);
  if (!root.is_object()) {
    return cache;
  }
  const auto version = root.find("version");
  const auto records = root.find("records");
  if (version == root.end() || !version->is_number_integer() ||
      *version != 2 || records == root.end() || !records->is_array()) {
    return cache;
  }
  for (const auto& item : *records) {
    if (!is_cache_record_json(item)) {
      return SemanticCache{};
    }
  }
  std::unordered_set<std::string> source_identities;
  source_identities.reserve(records->size());
  for (const auto& item : *records) {
    auto record = record_from_json(item);
    const auto source_identity =
        normalize_semantic_source_path(record.source_path);
    if (!source_identities.insert(source_identity).second) {
      return SemanticCache{};
    }
    cache.upsert(std::move(record));
  }
  return cache;
}

}  // namespace cgraph
