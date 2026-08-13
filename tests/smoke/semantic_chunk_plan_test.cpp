#include "cgraph/semantic_chunk_plan.hpp"

#include "cgraph/file_cache.hpp"
#include "cgraph/semantic_cache.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {

void write_file(const std::filesystem::path& path, std::string contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << contents;
}

void expect(bool& ok, bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ok = false;
  }
}

std::unordered_set<std::string> planned_paths(const cgraph::SemanticChunkPlan& plan) {
  std::unordered_set<std::string> paths;
  for (const auto& chunk : plan.chunks) {
    for (const auto& input : chunk.inputs) {
      paths.insert(input.path.generic_string());
    }
  }
  return paths;
}

}  // namespace

int main() {
  const auto root =
      std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() / "cgraph-semantic-chunk-plan-test");
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  bool ok = true;

  // --- Basic planning with v2 cache: valid, changed, new inputs ---------------
  {
    const auto cached_doc = root / "docs" / "cached.md";
    const auto changed_doc = root / "docs" / "changed.md";
    const auto new_doc = root / "docs" / "new.md";
    const auto media = root / "media" / "diagram.png";
    const auto code = root / "src" / "main.py";
    const auto ignored = root / "build" / "generated.md";
    const auto gitignored = root / "vendored" / "toolchain.md";
    const auto cached_fragment = root / "graphify-out" / "semantic" / "chunk_00.json";
    const auto changed_fragment = root / "graphify-out" / "semantic" / "chunk_old.json";

    write_file(root / ".gitignore", "/vendored/\n");
    write_file(cached_doc, "# Cached\nNo work needed\n");
    write_file(changed_doc, "# Changed\nBefore\n");
    write_file(new_doc, "# New\nNeeds work\n");
    write_file(media, "png bytes");
    write_file(code, "class CodeOnly:\n    pass\n");
    write_file(ignored, "# ignored\n");
    write_file(gitignored, "# vendored doc, must be skipped via .gitignore\n");
    write_file(cached_fragment, R"({"nodes":[],"edges":[]})");
    write_file(changed_fragment, R"({"nodes":[],"edges":[]})");

    cgraph::SemanticCache cache;
    // Valid record for cached_doc: source path + matching content hash + existing fragment
    cgraph::SemanticCacheRecord cached_rec;
    cached_rec.source_path = cached_doc;
    cached_rec.content_hash = cgraph::sha256_file_hex(cached_doc);
    cached_rec.fragment_path = cached_fragment;
    cached_rec.fragment_hash = cgraph::sha256_file_hex(cached_fragment);
    cached_rec.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(cached_rec);

    // Record for changed_doc with old hash (will be stale)
    cgraph::SemanticCacheRecord changed_rec;
    changed_rec.source_path = changed_doc;
    changed_rec.content_hash = cgraph::sha256_file_hex(changed_doc);
    changed_rec.fragment_path = changed_fragment;
    changed_rec.fragment_hash = cgraph::sha256_file_hex(changed_fragment);
    changed_rec.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(changed_rec);

    // Now change the content so the hash won't match
    write_file(changed_doc, "# Changed\nAfter\n");

    const auto plan = cgraph::plan_semantic_chunks(
        root, cache,
        cgraph::SemanticChunkPlanOptions{.max_files_per_chunk = 2, .max_bytes_per_chunk = 1024});
    const auto paths = planned_paths(plan);

    expect(ok, plan.cache_hits == 1, "basic: 1 cache hit for unchanged cached_doc");
    expect(ok, plan.chunks.size() == 2, "basic: 2 chunks planned");
    expect(ok, paths.size() == 3, "basic: 3 paths need work");
    expect(ok, !paths.contains(cached_doc.generic_string()), "basic: cached_doc not planned");
    expect(ok, !paths.contains(code.generic_string()), "basic: code not planned");
    expect(ok, !paths.contains(ignored.generic_string()), "basic: ignored not planned");
    expect(ok, !paths.contains(gitignored.generic_string()), "basic: gitignored not planned");
    expect(ok, paths.contains(changed_doc.generic_string()), "basic: changed_doc planned");
    expect(ok, paths.contains(new_doc.generic_string()), "basic: new_doc planned");
    expect(ok, paths.contains(media.generic_string()), "basic: media planned");
    expect(ok, plan.stale_inputs >= 1, "basic: at least 1 stale input");

    for (const auto& chunk : plan.chunks) {
      expect(ok, !chunk.inputs.empty() && chunk.inputs.size() <= 2, "basic: chunk size bounds");
    }
  }

  // --- Failed records are replanned (not treated as cache hits) ---------------
  {
    const auto fail_root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() / "cgraph-plan-failed-test");
    std::filesystem::remove_all(fail_root);

    const auto fail_doc = fail_root / "docs" / "fail.md";
    const auto fail_frag = fail_root / "semantic-drop" / "chunk_00.json";
    write_file(fail_doc, "# Failed\nContent\n");
    write_file(fail_frag, R"({"nodes":[],"edges":[]})");

    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = fail_doc;
    rec.content_hash = cgraph::sha256_file_hex(fail_doc);
    rec.fragment_path = fail_frag;
    rec.fragment_hash = cgraph::sha256_file_hex(fail_frag);
    rec.state = cgraph::SemanticCacheState::Failed;
    rec.last_error = "validation failed";
    cache.upsert(rec);

    const auto plan = cgraph::plan_semantic_chunks(fail_root, cache);
    const auto paths = planned_paths(plan);
    expect(ok, plan.cache_hits == 0, "failed: no cache hits for failed record");
    expect(ok, paths.contains(fail_doc.generic_string()), "failed: failed source is replanned");
    expect(ok, plan.failed_inputs == 1, "failed: failed_inputs count");

    std::filesystem::remove_all(fail_root);
  }

  // --- Stale records are replanned -------------------------------------------
  {
    const auto stale_root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() / "cgraph-plan-stale-test");
    std::filesystem::remove_all(stale_root);

    const auto stale_doc = stale_root / "docs" / "stale.md";
    const auto stale_frag = stale_root / "semantic-drop" / "chunk_00.json";
    write_file(stale_doc, "# Stale\nContent\n");
    write_file(stale_frag, R"({"nodes":[],"edges":[]})");

    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = stale_doc;
    rec.content_hash = cgraph::sha256_file_hex(stale_doc);
    rec.fragment_path = stale_frag;
    rec.state = cgraph::SemanticCacheState::Stale;
    rec.last_error = "dependency node missing";
    cache.upsert(rec);

    const auto plan = cgraph::plan_semantic_chunks(stale_root, cache);
    const auto paths = planned_paths(plan);
    expect(ok, plan.cache_hits == 0, "stale: no cache hits for stale record");
    expect(ok, paths.contains(stale_doc.generic_string()), "stale: stale source is replanned");
    expect(ok, plan.stale_inputs == 1, "stale: stale_inputs count");

    std::filesystem::remove_all(stale_root);
  }

  // --- Unrelated cache hits preserved across planning -------------------------
  {
    const auto pres_root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() / "cgraph-plan-preserve-test");
    std::filesystem::remove_all(pres_root);

    const auto preserved_doc = pres_root / "docs" / "preserved.md";
    const auto new_doc = pres_root / "docs" / "new.md";
    const auto pres_frag = pres_root / "semantic-drop" / "chunk_00.json";
    write_file(preserved_doc, "# Preserved\nStable\n");
    write_file(new_doc, "# New\nNeeds work\n");
    write_file(pres_frag, R"({"nodes":[],"edges":[]})");

    cgraph::SemanticCache cache;
    cgraph::SemanticCacheRecord rec;
    rec.source_path = preserved_doc;
    rec.content_hash = cgraph::sha256_file_hex(preserved_doc);
    rec.fragment_path = pres_frag;
    rec.fragment_hash = cgraph::sha256_file_hex(pres_frag);
    rec.state = cgraph::SemanticCacheState::Valid;
    cache.upsert(rec);

    const auto plan = cgraph::plan_semantic_chunks(pres_root, cache);
    const auto paths = planned_paths(plan);
    expect(ok, plan.cache_hits == 1, "preserve: valid record is a cache hit");
    expect(ok, !paths.contains(preserved_doc.generic_string()), "preserve: valid source not replanned");
    expect(ok, paths.contains(new_doc.generic_string()), "preserve: new source is planned");

    // Cache unchanged after planning
    const auto after = cache.find_for_source(preserved_doc);
    expect(ok, after.has_value() && after->state == cgraph::SemanticCacheState::Valid,
           "preserve: cache record untouched by planning");

    std::filesystem::remove_all(pres_root);
  }

  // --- Stat cache: unchanged files are not re-hashed across plans ------------
  {
    const auto stat_root = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path() / "cgraph-semantic-statcache-test");
    std::filesystem::remove_all(stat_root);
    write_file(stat_root / "a.md", "# A\n");
    write_file(stat_root / "b.md", "# B\n");
    write_file(stat_root / "c.md", "# C\n");

    cgraph::SemanticCache empty_cache;
    cgraph::SemanticStatIndex index;
    const auto cold = cgraph::plan_semantic_chunks(stat_root, empty_cache, {}, &index);
    expect(ok, cold.files_hashed == 3 && cold.files_stat_reused == 0 && index.size() == 3,
           "stat: cold plan hashes everything");

    const auto warm = cgraph::plan_semantic_chunks(stat_root, empty_cache, {}, &index);
    expect(ok, warm.files_hashed == 0 && warm.files_stat_reused == 3,
           "stat: unchanged files must not be re-hashed");
    expect(ok, planned_paths(warm) == planned_paths(cold) && warm.chunks.size() == cold.chunks.size(),
           "stat: plan output identical whether hashed or reused");

    write_file(stat_root / "b.md", "# B changed, now longer\n");
    const auto after_edit = cgraph::plan_semantic_chunks(stat_root, empty_cache, {}, &index);
    expect(ok, after_edit.files_hashed == 1 && after_edit.files_stat_reused == 2,
           "stat: only changed file re-hashed");

    const auto index_path = stat_root / "stat-index.json";
    cgraph::write_semantic_stat_index(index, index_path);
    auto reloaded = cgraph::read_semantic_stat_index(index_path);
    expect(ok, reloaded.size() == index.size(), "stat: index round-trip size");
    const auto after_reload = cgraph::plan_semantic_chunks(stat_root, empty_cache, {}, &reloaded);
    expect(ok, after_reload.files_hashed == 0 && after_reload.files_stat_reused == 3,
           "stat: restart over unchanged tree re-hashes nothing");

    auto absent = cgraph::read_semantic_stat_index(stat_root / "does-not-exist.json");
    const auto cold_again = cgraph::plan_semantic_chunks(stat_root, empty_cache, {}, &absent);
    expect(ok, cold_again.files_hashed == 3 && cold_again.files_stat_reused == 0,
           "stat: absent index file treated as cold");

    std::filesystem::remove_all(stat_root);
  }

  std::filesystem::remove_all(root);
  return ok ? 0 : 1;
}
