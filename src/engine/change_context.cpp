#include "cgraph/change_context.hpp"

#include "cgraph/daemon_ops.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/file_cache.hpp"
#include "cgraph/fragment_json.hpp"
#include "cgraph/pipeline.hpp"
#include "cgraph/snapshot_source_reader.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace cgraph {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Range { std::size_t start = 0; std::size_t count = 0; };
struct Hunk {
  Range old_range, new_range;
  std::vector<std::pair<char, std::string>> lines;
};
struct Change {
  std::string old_path, new_path;
  std::string rename_from, rename_to;
  bool old_header = false, new_header = false;
  std::vector<Hunk> hunks;
};

[[noreturn]] void reject(const std::string& reason) {
  throw std::invalid_argument("change-context: " + reason);
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  for (std::size_t pos = 0; pos < text.size();) {
    const auto end = text.find('\n', pos);
    const auto next = end == std::string::npos ? text.size() : end + 1;
    lines.push_back(text.substr(pos, next - pos));
    pos = next;
  }
  return lines;
}

std::string path_name(std::string value, bool header) {
  if (value == "/dev/null") return {};
  if (value.empty() || value.front() == '"' || value.find('\0') != std::string::npos)
    reject("empty or quoted diff path is unsupported");
  if (header) {
    const auto tab = value.find('\t');
    if (tab != std::string::npos) value.resize(tab);
    if (value.starts_with("a/") || value.starts_with("b/")) value.erase(0, 2);
  }
  const fs::path path(value);
  if (path.is_absolute() || value.empty()) reject("diff path must be repository relative");
  for (const auto& part : path) {
    if (part == ".." || part == ".") reject("diff path escapes or aliases its root");
  }
  if (value.find('\r') != std::string::npos) reject("carriage return in diff path");
  return path.generic_string();
}

std::vector<Change> parse_diff(const std::string& text) {
  const auto raw = lines_of(text);
  std::vector<std::string> lines;
  for (auto line : raw) {
    if (!line.empty() && line.back() == '\n') line.pop_back();
    lines.push_back(std::move(line));
  }
  std::vector<Change> changes;
  Change current;
  bool section = false;
  auto finish = [&] {
    if (!section) return;
    if (!current.rename_from.empty() || !current.rename_to.empty()) {
      if (current.rename_from.empty() || current.rename_to.empty()) reject("incomplete rename metadata");
      if (current.old_header && (current.old_path != current.rename_from || current.new_path != current.rename_to))
        reject("rename metadata disagrees with file headers");
      current.old_path = current.rename_from;
      current.new_path = current.rename_to;
    } else if (!current.old_header || !current.new_header) {
      reject("file section requires old/new headers (mode-only and binary changes unsupported)");
    }
    if (current.old_path.empty() && current.new_path.empty()) reject("both diff paths are absent");
    if (current.hunks.empty() && current.rename_from.empty()) reject("file section has no hunks");
    changes.push_back(std::move(current));
    current = Change{};
    section = false;
  };
  const std::regex header(R"(^@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@.*$)");
  for (std::size_t i = 0; i < lines.size();) {
    const auto& line = lines[i];
    if (line.starts_with("diff --git ")) {
      finish(); section = true; ++i; continue;
    }
    if (line.starts_with("--- ")) {
      if (current.old_header) finish();
      section = true;
      current.old_path = path_name(line.substr(4), true);
      current.old_header = true;
      if (++i >= lines.size() || !lines[i].starts_with("+++ ")) reject("missing new-file header");
      current.new_path = path_name(lines[i].substr(4), true);
      current.new_header = true;
      ++i; continue;
    }
    if (line.starts_with("@@")) {
      if (!current.old_header || !current.new_header) reject("hunk before file headers");
      std::smatch match;
      if (!std::regex_match(line, match, header)) reject("malformed hunk header");
      Hunk h;
      h.old_range = {std::stoull(match[1]), match[2].matched ? std::stoull(match[2]) : 1};
      h.new_range = {std::stoull(match[3]), match[4].matched ? std::stoull(match[4]) : 1};
      if ((h.old_range.count && !h.old_range.start) || (h.new_range.count && !h.new_range.start))
        reject("nonempty hunk starts at line zero");
      std::size_t old_count = 0, new_count = 0;
      ++i;
      while (i < lines.size()) {
        const auto& body = lines[i];
        if (body == "\\ No newline at end of file") {
          if (h.lines.empty() || h.lines.back().second.empty() || h.lines.back().second.back() != '\n')
            reject("misplaced no-newline marker");
          h.lines.back().second.pop_back(); ++i; continue;
        }
        if (old_count == h.old_range.count && new_count == h.new_range.count) break;
        if (body.empty() || (body.front() != ' ' && body.front() != '+' && body.front() != '-'))
          reject("truncated or malformed hunk body");
        const auto kind = body.front();
        old_count += kind != '+';
        new_count += kind != '-';
        if (old_count > h.old_range.count || new_count > h.new_range.count) reject("hunk count overflow");
        h.lines.emplace_back(kind, body.substr(1) + '\n');
        ++i;
      }
      if (old_count != h.old_range.count || new_count != h.new_range.count) reject("hunk count mismatch");
      current.hunks.push_back(std::move(h));
      continue;
    }
    if (line.starts_with("rename from ")) current.rename_from = path_name(line.substr(12), false);
    else if (line.starts_with("rename to ")) current.rename_to = path_name(line.substr(10), false);
    else if (!(line.starts_with("index ") || line.starts_with("similarity index ") ||
               line.starts_with("new file mode ") || line.starts_with("deleted file mode ")))
      reject("unsupported diff record: " + line.substr(0, 80));
    if (!section) reject("metadata outside a file section");
    ++i;
  }
  finish();
  if (changes.empty()) reject("diff contains no changes");
  std::set<std::string> old_paths, new_paths;
  for (const auto& change : changes) {
    if ((!change.old_path.empty() && !old_paths.insert(change.old_path).second) ||
        (!change.new_path.empty() && !new_paths.insert(change.new_path).second))
      reject("duplicate file section");
  }
  return changes;
}

fs::path source_path(const fs::path& root, const std::string& path) {
  const auto resolved = fs::weakly_canonical(root / path);
  const auto relative = resolved.lexically_relative(root);
  if (relative.empty() || relative.is_absolute()) reject("source path is outside its root");
  for (const auto& part : relative) if (part == "..") reject("source symlink escapes its root");
  return resolved;
}

void validate_patch(const Change& change, const std::string& old_text, const std::string& new_text) {
  const auto old_lines = lines_of(old_text);
  const auto new_lines = lines_of(new_text);
  std::vector<std::string> output;
  std::size_t consumed = 0;
  for (const auto& hunk : change.hunks) {
    const auto old_pos = hunk.old_range.count ? hunk.old_range.start - 1 : hunk.old_range.start;
    const auto new_pos = hunk.new_range.count ? hunk.new_range.start - 1 : hunk.new_range.start;
    if (old_pos < consumed || old_pos > old_lines.size()) reject("overlapping or out-of-range hunk");
    output.insert(output.end(), old_lines.begin() + static_cast<std::ptrdiff_t>(consumed),
                  old_lines.begin() + static_cast<std::ptrdiff_t>(old_pos));
    consumed = old_pos;
    if (output.size() != new_pos) reject("new hunk position disagrees with preceding edits");
    for (const auto& [kind, text] : hunk.lines) {
      if (kind != '+') {
        if (consumed >= old_lines.size() || old_lines[consumed] != text)
          reject("old hunk content does not match base source: " + change.old_path);
        ++consumed;
      }
      if (kind != '-') output.push_back(text);
    }
  }
  output.insert(output.end(), old_lines.begin() + static_cast<std::ptrdiff_t>(consumed), old_lines.end());
  if (output != new_lines) reject("diff does not reproduce target source: " + change.new_path);
}

// Changed lines only, not unchanged hunk context. Zero-count ranges denote the
// insertion/deletion boundary on the side without corresponding lines.
std::vector<Range> changed_ranges(const Change& change, bool old_side) {
  std::vector<Range> ranges;
  for (const auto& hunk : change.hunks) {
    std::size_t old_line = hunk.old_range.start, new_line = hunk.new_range.start;
    for (const auto& [kind, _] : hunk.lines) {
      if ((old_side && kind == '-') || (!old_side && kind == '+')) {
        const auto line = old_side ? old_line : new_line;
        if (!ranges.empty() && ranges.back().start + ranges.back().count == line) ++ranges.back().count;
        else ranges.push_back({line, 1});
      }
      if ((old_side && kind == '+') || (!old_side && kind == '-')) {
        const auto line = std::max<std::size_t>(1, old_side ? old_line : new_line);
        ranges.push_back({line, 0});
      }
      old_line += kind != '+';
      new_line += kind != '-';
    }
  }
  return ranges;
}

Json ranges_json(const std::vector<Range>& ranges) {
  auto out = Json::array();
  for (const auto& range : ranges) out.push_back({{"start", range.start}, {"count", range.count}});
  return out;
}

std::vector<std::string> match_symbols(const GraphSnapshot& graph, const fs::path& path,
                                     const std::vector<Range>& ranges, bool entire_file) {
  std::vector<std::string> ids;
  std::string file_id;
  std::set<std::size_t> covered_lines;
  for (const auto& node : graph.nodes) {
    if (fs::path(node.source_file).lexically_normal() != path) continue;
    if (node.kind == "file") { file_id = node.id; continue; }
    if (!node.source_location) continue;
    const auto& location = *node.source_location;
    bool overlaps = entire_file;
    for (const auto& range : ranges) {
      const auto count = std::max<std::size_t>(1, range.count);
      const auto last = range.start + count - 1;
      overlaps |= location.start_line <= last && location.end_line >= range.start;
      for (auto line = std::max<std::size_t>(location.start_line, range.start);
           line <= std::min<std::size_t>(location.end_line, last); ++line) covered_lines.insert(line);
    }
    if (overlaps) ids.push_back(node.id);
  }
  // File-level imports/re-exports and edits outside a symbol use the real file
  // node as their seed; absence of a symbol never invents a structural edge.
  bool outside_symbol = ranges.empty();
  for (const auto& range : ranges) {
    for (std::size_t line = range.start; line < range.start + std::max<std::size_t>(1, range.count); ++line)
      outside_symbol |= !covered_lines.contains(line);
  }
  if ((ids.empty() || entire_file || outside_symbol) && !file_id.empty()) ids.push_back(file_id);
  std::ranges::sort(ids);
  return ids;
}

Json symbol_changes(const GraphSnapshot& base, const GraphSnapshot& target,
                    const std::vector<std::string>& old_ids, const std::vector<std::string>& new_ids,
                    bool file_renamed) {
  const auto signatures = [](const GraphSnapshot& graph) {
    std::unordered_map<std::string, const Node*> nodes;
    std::unordered_map<std::string, std::vector<std::string>> parents;
    for (const auto& node : graph.nodes) nodes.emplace(node.id, &node);
    for (const auto& edge : graph.edges)
      if (edge.relation == "contains" || edge.relation == "method" || edge.relation == "defines") parents[edge.target].push_back(edge.source);
    std::map<std::string, std::string> keys;
    for (const auto& node : graph.nodes) {
      std::string key = node.kind + ":" + node.label;
      auto cursor = node.id;
      std::set<std::string> visited;
      while (visited.insert(cursor).second) {
        const auto parent = parents.find(cursor);
        if (parent == parents.end()) break;
        if (parent->second.size() != 1 || !nodes.contains(parent->second.front())) {
          key += ":ambiguous-owner:" + node.id; break;
        }
        const auto* owner = nodes.at(parent->second.front());
        if (owner->kind == "file") break;
        key = owner->kind + ":" + owner->label + "/" + key;
        cursor = owner->id;
      }
      keys.emplace(node.id, std::move(key));
    }
    return keys;
  };
  const auto old_keys = signatures(base), new_keys = signatures(target);
  std::map<std::string, const Node*> old_nodes, new_nodes;
  for (const auto& node : base.nodes)
    if (node.kind != "file" && std::ranges::find(old_ids, node.id) != old_ids.end()) old_nodes.emplace(node.id, &node);
  for (const auto& node : target.nodes)
    if (node.kind != "file" && std::ranges::find(new_ids, node.id) != new_ids.end()) new_nodes.emplace(node.id, &node);
  auto out = Json::array();
  std::set<std::string> paired;
  const auto ref = [](const Node& node) {
    return Json{{"id", node.id}, {"label", node.label}, {"kind", node.kind},
                {"line", node.source_location ? node.source_location->start_line : 0}};
  };
  for (const auto& [id, old_node] : old_nodes) {
    std::vector<const Node*> matches;
    for (const auto& [_, node] : new_nodes)
      if (new_keys.at(node->id) == old_keys.at(id)) matches.push_back(node);
    const auto old_count = std::count_if(base.nodes.begin(), base.nodes.end(), [&](const auto& node) {
      return node.source_file == old_node->source_file && old_keys.at(node.id) == old_keys.at(id);
    });
    const auto new_count = matches.size() != 1 ? 0 : std::count_if(target.nodes.begin(), target.nodes.end(), [&](const auto& node) {
      return node.source_file == matches.front()->source_file && new_keys.at(node.id) == new_keys.at(matches.front()->id);
    });
    if (matches.size() == 1 && old_count == 1 && new_count == 1) {
      paired.insert(matches.front()->id);
      out.push_back({{"status", file_renamed ? "moved" : "changed"},
                     {"base", ref(*old_node)}, {"target", ref(*matches.front())},
                     {"pairing", "unique_qualified_owner_label_and_kind_within_diff_file"}});
    } else {
      out.push_back({{"status", new_nodes.empty() ? "deleted" : "deleted_or_renamed"}, {"base", ref(*old_node)},
                     {"target", nullptr}, {"pairing", "unresolved"}});
    }
  }
  for (const auto& [id, node] : new_nodes) if (!paired.contains(id))
    out.push_back({{"status", old_nodes.empty() ? "added" : "added_or_renamed"}, {"base", nullptr}, {"target", ref(*node)},
                   {"pairing", "unresolved"}});
  return out;
}

void verify_snapshot(const PipelineResult& pipeline, const fs::path& root) {
  const auto files = detect_project_files(root);
  if (files.size() != pipeline.graph.source_hashes.size())
    reject("source coverage changed or a detected file could not be read");
  SnapshotSourceReader reader(pipeline.graph.source_hashes, true);
  for (const auto& file : files) (void)reader.read_verified_source(file.path.generic_string());
}

Json root_json(const PipelineResult& pipeline, const fs::path& root) {
  return {{"root", root.generic_string()}, {"content_root", pipeline.graph.content_root.sha256},
          {"algorithm", pipeline.graph.content_root.algorithm},
          {"indexed_files", pipeline.graph.source_hashes.size()}};
}

void check_pin(const Json& parameters, const char* key, const GraphSnapshot& graph) {
  if (parameters.contains(key) && parameters.at(key).get<std::string>() != graph.content_root.sha256)
    reject(std::string(key) + " does not match source snapshot");
}

Json uncertainty(const PipelineResult& pipeline) {
  auto stats = build_stats_json(pipeline.stats);
  return {{"unsupported_languages", pipeline.stats.unextracted},
          {"warnings", pipeline.warnings}, {"call_resolution", stats.at("call_resolution")}};
}

// The endpoints a change touches, each with the roles it plays for them and a
// rank: 0 for an endpoint the change edits, removes or adds, 1 for one whose
// handler it reaches or that changed code calls, 2 for one a direct caller of
// changed code calls. Asking and trimming both go in rank order.
struct Touch {
  std::set<std::string> roles;
  int rank = 4;
  bool outside_diff = false;  // only from root differences the diff does not supply
};
using TouchedContracts = std::map<std::string, Touch>;

void touch(TouchedContracts& touched, const std::string& id, const std::string& role, int rank) {
  auto& entry = touched[id];
  entry.roles.insert(role);
  entry.rank = std::min(entry.rank, rank);
}

// Endpoints a graph serves: those with a handler or a declaring file.
std::set<std::string> served_endpoints(const GraphSnapshot& graph) {
  std::set<std::string> served;
  for (const auto& edge : graph.edges) {
    if (edge.relation == "handled_by" && edge.source.starts_with("endpoint:")) served.insert(edge.source);
    if (edge.relation == "contains" && edge.target.starts_with("endpoint:")) served.insert(edge.target);
  }
  return served;
}

constexpr std::size_t kMaxCrossServiceContracts = 24;

// Asks every other repo of the workspace who consumes what this change serves
// (or removed, or added) and who provides what it calls. A repo that cannot
// answer is asked once and named, as is one answering from a graph still
// building, so an empty list means "none found", not "unknown".
Json cross_service_section(const CrossServiceAsk& scope, const TouchedContracts& touched) {
  const auto& enclosing = *scope.enclosing;
  Json section{{"workspace", enclosing.workspace.name}, {"home", enclosing.home},
               {"contracts", Json::array()}, {"rows", Json::array()},
               {"unreachable", Json::array()}, {"building", Json::array()}};
  if (!enclosing.workspace.ok()) {
    section["errors"] = enclosing.workspace.errors;
    return section;
  }
  std::vector<std::pair<std::string, const Touch*>> ordered;
  for (const auto& [id, entry] : touched) ordered.emplace_back(id, &entry);
  std::ranges::stable_sort(ordered, [](const auto& a, const auto& b) { return a.second->rank < b.second->rank; });
  if (ordered.size() > kMaxCrossServiceContracts) {
    section["contracts_omitted"] = ordered.size() - kMaxCrossServiceContracts;
    ordered.resize(kMaxCrossServiceContracts);
  }
  std::set<std::string> unreachable, building;
  for (const auto& [contract, entry] : ordered) {
    Json listed{{"id", contract}, {"roles", Json(entry->roles)}, {"rank", entry->rank}};
    if (entry->outside_diff) listed["outside_diff"] = true;
    section["contracts"].push_back(std::move(listed));
    const bool served = entry->roles.contains("serves") || entry->roles.contains("removed") || entry->roles.contains("added");
    const bool called = entry->roles.contains("consumes");
    for (const bool consumers : {true, false}) {
      if ((consumers && !served) || (!consumers && !called)) continue;
      for (const auto& repo : enclosing.workspace.repos) {
        if (repo.name == enclosing.home || unreachable.contains(repo.name)) continue;
        std::string error;
        // Direct callers of what this change serves (CONSUMES), and the handler
        // behind what it calls (handled_by): the code another team would touch.
        const Json params{{"id", contract}, {"direction", consumers ? "dependents" : "dependencies"},
                          {"relation", consumers ? "CONSUMES" : "handled_by"}, {"max_depth", 1}};
        const auto envelope = scope.ask(repo, "impact", params, error);
        if (!envelope || !envelope->value("ok", false)) {
          unreachable.insert(repo.name);
          section["unreachable"].push_back({{"repo", repo.name},
              {"error", envelope ? envelope->value("error", std::string{"request failed"}) : error}});
          continue;
        }
        const auto& answer = envelope->at("result");
        if (answer.value("graph_state", std::string{}) == "building" && building.insert(repo.name).second)
          section["building"].push_back(repo.name);
        for (const auto& node : answer.value("nodes", Json::array())) {
          const auto id = node.value("id", std::string{});
          if (id.starts_with("endpoint:")) continue;
          const auto file = node.value("source_file", std::string{});
          section["rows"].push_back({{"contract", contract}, {"rank", entry->rank},
              {"relation", consumers ? "consumer" : "provider"},
              {"repo", repo.name}, {"id", id}, {"label", node.value("label", std::string{})},
              {"kind", node.value("kind", std::string{})},
              {"path", file.empty() ? "" : fs::path(file).lexically_relative(repo.root).generic_string()},
              {"line", node.value("line", 0)}});
        }
      }
    }
  }
  auto& rows = section["rows"];
  std::stable_sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
    return std::tuple(a.at("rank").get<int>(), a.at("contract").get<std::string>(), a.at("repo").get<std::string>(),
                      a.at("path").get<std::string>(), a.at("line").get<int>()) <
           std::tuple(b.at("rank").get<int>(), b.at("contract").get<std::string>(), b.at("repo").get<std::string>(),
                      b.at("path").get<std::string>(), b.at("line").get<int>());
  });
  return section;
}

}  // namespace

Json change_context(const Json& parameters, const CrossServiceAsk* cross_service) {
  const auto base_root = fs::canonical(parameters.at("base_root").get<std::string>());
  const auto target_root = fs::canonical(parameters.at("target_root").get<std::string>());
  if (!fs::is_directory(base_root) || !fs::is_directory(target_root)) reject("roots must be directories");
  const auto budget_signed = parameters.value("budget", 6000LL);
  const auto max_depth = parameters.value("max_depth", 3);
  // symbols_only answers "which symbols did this diff change" and nothing else:
  // no impacts, no context, and no budget shedding, so every symbol change is
  // returned however large the diff.
  const bool symbols_only = parameters.value("symbols_only", false);
  if (budget_signed < 128 || budget_signed > 1000000) reject("budget must be between 128 and 1000000");
  if (max_depth < 0 || max_depth > 20) reject("max_depth must be between 0 and 20");
  const auto budget = static_cast<std::size_t>(budget_signed);
  const std::unordered_map<std::string, std::string> no_hashes;
  SnapshotSourceReader input_reader(no_hashes, false);
  const auto diff_path = fs::canonical(parameters.at("diff_path").get<std::string>()).generic_string();
  const auto diff_text = input_reader.read_verified_source(diff_path);
  const auto changes = parse_diff(diff_text);
  auto base = run_one_shot(base_root);
  auto target = run_one_shot(target_root);
  check_pin(parameters, "expected_base_content_root", base.graph);
  check_pin(parameters, "expected_target_content_root", target.graph);

  Json result{{"schema_version", 1}, {"advisory", true}, {"scope", "supplied_diff"},
      {"base", root_json(base, base_root)}, {"target", root_json(target, target_root)},
      {"diff_sha256", sha256_hex(diff_text)}, {"changes", Json::array()},
      {"impacts", Json::array()}, {"context", Json::array()},
      {"uncertainty", {{"base", uncertainty(base)}, {"target", uncertainty(target)},
          {"limitations", {"Unresolved calls are aggregate counts, not per-change diagnostics.",
                           "Impact is advisory and bounded by depth; dynamic dependencies may be absent.",
                           "Only supplied diff sections are assessed; other root differences are not inferred."}}}},
      {"budget", budget}, {"budget_basis", "serialized_utf8_bytes_div_4_ceil"}, {"symbols_only", symbols_only},
      {"tokens_used", 0}, {"omitted", {{"context", 0}, {"impacts", 0}}}, {"truncated", false}};
  std::vector<std::string> base_seeds, target_seeds;
  std::map<std::string, std::string> diff_hashes;
  for (const auto& change : changes) {
    std::string old_text, new_text;
    fs::path old_path, new_path;
    if (!change.old_path.empty()) {
      old_path = source_path(base_root, change.old_path);
      old_text = input_reader.read_verified_source(old_path.generic_string());
      diff_hashes[old_path.generic_string()] = sha256_hex(old_text);
    }
    if (!change.new_path.empty()) {
      new_path = source_path(target_root, change.new_path);
      new_text = input_reader.read_verified_source(new_path.generic_string());
      diff_hashes[new_path.generic_string()] = sha256_hex(new_text);
    }
    if (change.old_path.empty() && fs::exists(source_path(base_root, change.new_path)))
      reject("added file already exists in base");
    if (change.new_path.empty() && fs::exists(source_path(target_root, change.old_path)))
      reject("deleted file still exists in target");
    validate_patch(change, old_text, new_text);
    const auto old_ranges = changed_ranges(change, true), new_ranges = changed_ranges(change, false);
    const bool rename = !change.old_path.empty() && !change.new_path.empty() && change.old_path != change.new_path;
    if (rename && (fs::exists(source_path(target_root, change.old_path)) ||
                   fs::exists(source_path(base_root, change.new_path))))
      reject("rename source must disappear and destination must be new");
    auto old_ids = change.old_path.empty() ? std::vector<std::string>{} :
        match_symbols(base.graph, old_path, old_ranges, rename || change.new_path.empty());
    auto new_ids = change.new_path.empty() ? std::vector<std::string>{} :
        match_symbols(target.graph, new_path, new_ranges, rename || change.old_path.empty());
    base_seeds.insert(base_seeds.end(), old_ids.begin(), old_ids.end());
    target_seeds.insert(target_seeds.end(), new_ids.begin(), new_ids.end());
    const auto deltas = symbol_changes(base.graph, target.graph, old_ids, new_ids, rename);
    result["changes"].push_back({{"old_path", change.old_path}, {"new_path", change.new_path},
        {"status", change.old_path.empty() ? "added" : change.new_path.empty() ? "deleted" : rename ? "renamed" : "modified"},
        {"old_ranges", ranges_json(old_ranges)}, {"new_ranges", ranges_json(new_ranges)},
        {"base_symbols", old_ids}, {"target_symbols", new_ids}, {"symbol_changes", deltas},
        {"base_mapped", !old_ids.empty()}, {"target_mapped", !new_ids.empty()},
        {"old_sha256", change.old_path.empty() ? "" : sha256_hex(old_text)},
        {"new_sha256", change.new_path.empty() ? "" : sha256_hex(new_text)}});
  }

  std::map<std::string, std::string> base_inventory, target_inventory;
  std::set<std::string> supplied, all_paths;
  for (const auto& change : changes) {
    if (!change.old_path.empty()) supplied.insert(change.old_path);
    if (!change.new_path.empty()) supplied.insert(change.new_path);
  }
  for (const auto& [path, hash] : base.graph.source_hashes) {
    const auto relative = fs::path(path).lexically_relative(base_root).generic_string();
    base_inventory[relative] = hash; all_paths.insert(relative);
  }
  for (const auto& [path, hash] : target.graph.source_hashes) {
    const auto relative = fs::path(path).lexically_relative(target_root).generic_string();
    target_inventory[relative] = hash; all_paths.insert(relative);
  }
  auto outside = Json::array();
  for (const auto& path : all_paths) {
    if (!supplied.contains(path) && base_inventory[path] != target_inventory[path]) outside.push_back(path);
  }
  result["uncertainty"]["unassessed_source_paths"] = outside;
  result["uncertainty"]["unassessed_source_count"] = outside.size();
  result["uncertainty"]["all_detected_differences_supplied"] = outside.empty();

  const auto count_tokens = [&] {
    for (int i = 0; i < 3; ++i) result["tokens_used"] = serialized_context_tokens(result);
    return result["tokens_used"].get<std::size_t>();
  };
  // Reopen sources after selection; a cached old buffer must not conceal edits
  // made while this operation was building its other snapshot or packing.
  const auto verify_sources = [&] {
    verify_snapshot(base, base_root);
    verify_snapshot(target, target_root);
    const std::unordered_map<std::string, std::string> diff_ledger(diff_hashes.begin(), diff_hashes.end());
    SnapshotSourceReader diff_verifier(diff_ledger, true);
    for (const auto& [path, _] : diff_hashes) (void)diff_verifier.read_verified_source(path);
  };
  if (symbols_only) {
    count_tokens();
    verify_sources();
    return result;
  }

  GraphSnapshot combined;
  std::vector<std::string> combined_seeds;
  struct Origin { const Node* node; std::string snapshot; fs::path root; };
  std::unordered_map<std::string, Origin> origins;
  TouchedContracts touched;
  const auto add_side = [&](const PipelineResult& pipeline, const fs::path& root,
                            const std::string& side, std::vector<std::string>& seeds) {
    std::ranges::sort(seeds);
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    const auto reached = trace_impact(pipeline.graph, seeds, "dependents", "", max_depth);
    std::unordered_map<std::string, const Node*> by_id;
    for (const auto& node : pipeline.graph.nodes) by_id.emplace(node.id, &node);
    // An endpoint the change serves: one it changed, one whose handler it
    // reached (the last step is handled_by), or one a changed file contains.
    // Not every endpoint reachable through the app's router mounts: a router
    // chain spans its whole file, and its importers reach every mounted route.
    for (const auto& [id, reach] : reached) {
      if (!id.starts_with("endpoint:")) continue;
      if (reach.depth == 0) touch(touched, id, "serves", 0);
      else if (reach.via == "handled_by" || (reach.depth == 1 && reach.via == "contains")) touch(touched, id, "serves", 1);
    }
    // An endpoint the change calls: CONSUMES from changed code, or from a
    // function that calls a changed helper directly (its request may change).
    // Callers further out call other endpoints for their own reasons.
    for (const auto& edge : pipeline.graph.edges) {
      if (edge.relation != "CONSUMES" || !edge.target.starts_with("endpoint:")) continue;
      const auto reach = reached.find(edge.source);
      if (reach == reached.end()) continue;
      if (reach->second.depth == 0) touch(touched, edge.target, "consumes", 1);
      else if (reach->second.depth == 1 && reach->second.via == "CALLS") touch(touched, edge.target, "consumes", 2);
    }
    const auto brief = [&](const std::string& id) {
      const auto& node = *by_id.at(id);
      Json item{{"id", id}, {"label", node.label}, {"kind", node.kind},
          {"path", fs::path(node.source_file).lexically_relative(root).generic_string()},
          {"line", node.source_location ? node.source_location->start_line : 0}};
      if (const auto hash = pipeline.graph.source_hashes.find(node.source_file); hash != pipeline.graph.source_hashes.end())
        item["source_sha256"] = hash->second;
      return item;
    };
    std::vector<std::string> impacts;
    for (const auto& [id, reach] : reached) if (reach.depth > 0 && by_id.contains(id)) impacts.push_back(id);
    std::ranges::sort(impacts, [&](const auto& a, const auto& b) {
      return std::tie(reached.at(a).depth, a) < std::tie(reached.at(b).depth, b);
    });
    for (const auto& id : impacts) {
      const auto& reach = reached.at(id);
      auto item = brief(id);
      item.update({{"snapshot", side}, {"depth", reach.depth}, {"via", reach.via},
                   {"changed_id", reach.changed_id}});
      auto witness = Json::array();
      for (const auto& step : reach.witness) {
        if (!by_id.contains(step.source) || !by_id.contains(step.target)) break;
        witness.push_back({{"snapshot", side}, {"source", brief(step.source)},
            {"target", brief(step.target)}, {"relation", step.relation},
            {"confidence", confidence_to_string(step.confidence)}});
      }
      item["witness"] = std::move(witness);
      result["impacts"].push_back(std::move(item));
    }
    // Namespacing is local to the union graph; original graph IDs are untouched.
    const auto prefix = side + "::";
    for (auto node : pipeline.graph.nodes) {
      const auto* original = by_id.at(node.id);
      node.id = prefix + node.id;
      origins.emplace(node.id, Origin{original, side, root});
      combined.nodes.push_back(std::move(node));
    }
    for (auto edge : pipeline.graph.edges) {
      edge.source = prefix + edge.source; edge.target = prefix + edge.target;
      combined.edges.push_back(std::move(edge));
    }
    for (const auto& [path, hash] : pipeline.graph.source_hashes) {
      if (const auto prior = combined.source_hashes.find(path); prior != combined.source_hashes.end() && prior->second != hash)
        reject("overlapping roots have inconsistent source snapshots");
      combined.source_hashes[path] = hash;
    }
    for (const auto& seed : seeds) combined_seeds.push_back(prefix + seed);
  };
  add_side(base, base_root, "base", base_seeds);
  add_side(target, target_root, "target", target_seeds);
  // A route whose path the change moves or deletes (a mount prefix, a renamed
  // path, a removed handler) is served before and not after: its consumers are
  // exactly the code that breaks. A newly served route is asked about too.
  // When the roots differ in files the diff does not supply, a route may have
  // moved for reasons outside the diff: it is still named, marked, and ranked
  // after everything the diff itself touches.
  {
    const bool complete = outside.empty();
    const auto mark = [&](const std::string& id, const std::string& role) {
      const bool known = touched.contains(id);
      touch(touched, id, role, complete ? 0 : 3);
      if (!complete && !known) touched[id].outside_diff = true;
    };
    const auto before = served_endpoints(base.graph), after = served_endpoints(target.graph);
    for (const auto& id : before) if (!after.contains(id)) mark(id, "removed");
    for (const auto& id : after) if (!before.contains(id)) mark(id, "added");
  }
  // Other services' consumers and providers are never shed to make room for
  // impacts. They get at most a quarter of the budget, trimmed lowest rank
  // first (rows, then contracts), and they never cause a rejection: at worst
  // the section shrinks to a stub that says how much it held.
  std::size_t cross_rows_total = 0;
  if (cross_service != nullptr && cross_service->enclosing != nullptr) {
    auto section = cross_service_section(*cross_service, touched);
    cross_rows_total = section["rows"].size();
    const auto cap = budget / 4;
    const auto section_tokens = [&] { return (section.dump().size() + 3) / 4; };
    while (section_tokens() > cap && !section["rows"].empty()) section["rows"].erase(section["rows"].end() - 1);
    while (section_tokens() > cap && !section["contracts"].empty()) {
      section["contracts"].erase(section["contracts"].end() - 1);
      section["contracts_omitted"] = section.value("contracts_omitted", std::size_t{0}) + 1;
    }
    const auto trimmed = cross_rows_total - section["rows"].size();
    result["omitted"]["cross_service"] = trimmed;
    result["truncated"] = result["truncated"].get<bool>() || trimmed > 0;
    result["cross_service"] = std::move(section);
  }
  // Shrinks the section when mandatory evidence needs the room: first to a
  // stub naming what was found, then away entirely. True while it could.
  const auto relieve_cross_service = [&] {
    if (!result.contains("cross_service")) return false;
    auto& section = result["cross_service"];
    if (!section.value("stub", false)) {
      // Same shape, emptied: `stub` says the lists were cut for budget, and the
      // counts say how much there was, so empty lists are never read as "none".
      section = Json{{"workspace", section.at("workspace")}, {"home", section.at("home")}, {"stub", true},
                     {"contracts", Json::array()}, {"rows", Json::array()},
                     {"contracts_found", section.at("contracts").size()}, {"rows_found", cross_rows_total}};
      result["omitted"]["cross_service"] = cross_rows_total;
      result["truncated"] = true;
    } else {
      // Gone entirely: leave nothing of it, so the answer is no larger than the
      // change alone would be.
      result.erase("cross_service");
      result["omitted"].erase("cross_service");
    }
    return true;
  };

  // Keep the mandatory diff mapping and uncertainty intact. Shed the deepest
  // impact witnesses first; context gets one shared remaining allowance.
  const auto remove_impact = [&] {
    auto& rows = result["impacts"];
    auto deepest = std::max_element(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
      return a.at("depth").get<int>() < b.at("depth").get<int>();
    });
    rows.erase(deepest);
    result["omitted"]["impacts"] = result["omitted"]["impacts"].get<std::size_t>() + 1;
    result["truncated"] = true;
  };
  while (count_tokens() > budget / 2 && !result["impacts"].empty()) remove_impact();
  while (count_tokens() >= budget && relieve_cross_service()) {}
  if (count_tokens() >= budget) reject("budget cannot hold mandatory change and snapshot evidence");
  if (!combined_seeds.empty()) {
    SnapshotSourceReader reader(combined.source_hashes, true);
    auto context = pack_seed_context(combined, combined_seeds, budget - count_tokens(), max_depth, reader);
    auto entries = Json::array();
    if (!context.at("focus").is_null()) entries.push_back(context.at("focus"));
    for (const auto& entry : context.at("included")) entries.push_back(entry);
    for (auto entry : entries) {
      const auto& origin = origins.at(entry.at("id").get<std::string>());
      entry["id"] = origin.node->id;
      entry["snapshot"] = origin.snapshot;
      entry["path"] = fs::path(origin.node->source_file).lexically_relative(origin.root).generic_string();
      entry.erase("source_file");
      result["context"].push_back(std::move(entry));
    }
    result["omitted"]["context"] = context.value("omitted", std::size_t{0});
    result["truncated"] = result["truncated"].get<bool>() || context.value("truncated", false);
  }
  while (count_tokens() > budget && !result["context"].empty()) {
    result["context"].erase(result["context"].end() - 1);
    result["omitted"]["context"] = result["omitted"]["context"].get<std::size_t>() + 1;
    result["truncated"] = true;
  }
  while (count_tokens() > budget && relieve_cross_service()) {}
  if (count_tokens() > budget) reject("budget cannot hold mandatory change and snapshot evidence");
  verify_sources();
  return result;
}

}  // namespace cgraph
