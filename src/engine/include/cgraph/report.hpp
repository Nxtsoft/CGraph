#pragma once

#include "cgraph/types.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// Structural reports (report-modules): one daemon op, `report`, with a `view`
// selector. Only `modules` is implemented; the other views are reserved so the
// tool surface an agent discovers is stable while they land (CGR-9/10/11).
enum class ReportView { Modules, Design, Clones, Types };
enum class ReportFormat { Json, Mermaid, Svg, Markdown };

[[nodiscard]] const char* report_view_name(ReportView view);
[[nodiscard]] std::optional<ReportView> report_view_from_string(std::string_view name);
[[nodiscard]] const char* report_format_name(ReportFormat format);
[[nodiscard]] std::optional<ReportFormat> report_format_from_string(std::string_view name);

// Token budget the daemon applies when a request names none. 0 disables the
// budget (the one-shot export writes the whole diagram).
inline constexpr std::size_t kDefaultReportBudget = 6000;
inline constexpr int kDefaultModuleDepth = 2;

struct ReportRequest {
  ReportView view = ReportView::Modules;
  ReportFormat format = ReportFormat::Json;
  std::size_t budget = kDefaultReportBudget;
  // Root-relative path prefix ("src"): only modules under it have their outgoing
  // dependencies reported; the modules they depend on appear as targets even
  // when they lie outside the prefix. Empty means every non-test module.
  std::string scope;
  // Test roots (tests/, __tests__/, spec/, e2e/, testdata/, fixtures/) are
  // excluded as sources unless asked for: on CGraph itself tests/smoke sends
  // hundreds of calls into src/engine and would own layer 0.
  bool include_tests = false;
  // Directory components that name a module: depth 2 turns
  // src/engine/dedup.cpp into src/engine.
  int module_depth = kDefaultModuleDepth;
  // Reserved for the clones view (CGR-10): similarity floor and token floor.
  double threshold = 0.80;
  std::size_t min_tokens = 30;
  // Module names are relative to this root; empty leaves paths as they are.
  std::filesystem::path project_root;
};

// Fills `out` from a daemon `params` object. Returns an error message for an
// unknown view/format or a negative depth; a mistyped value throws
// nlohmann::json::exception like every other op's parameter read.
[[nodiscard]] std::optional<std::string> parse_report_request(const nlohmann::json& params, ReportRequest& out);

struct ModuleSummary {
  std::string name;
  std::size_t files = 0;
  std::size_t symbols = 0;
  int layer = 0;
  bool in_cycle = false;
};

struct ModuleDependency {
  std::string from;
  std::string to;
  std::size_t calls = 0;
  std::size_t imports = 0;  // imports + imports_from + re_exports
  bool cycle = false;       // both ends share a strongly connected component
  [[nodiscard]] std::size_t weight() const { return calls + imports; }
};

struct ModulesReport {
  int depth = kDefaultModuleDepth;
  std::string scope;
  bool include_tests = false;
  std::vector<ModuleSummary> modules;          // by layer, then weight, then name
  std::vector<ModuleDependency> edges;         // heaviest first
  std::vector<std::vector<std::string>> layers;  // layer index -> module names
  std::vector<std::vector<std::string>> cycles;  // each strongly connected component with >1 module
  std::size_t omitted_modules = 0;
  std::size_t omitted_edges = 0;
  std::size_t total_modules = 0;
  std::size_t total_edges = 0;
};

// Groups every code node by the first `module_depth` directory components of
// its source file, aggregates imports/imports_from/re_exports/CALLS edges
// between groups, ranks layers by longest path over the module DAG, and lists
// cycles (strongly connected components). No budget is applied here.
[[nodiscard]] ModulesReport build_modules_report(const GraphSnapshot& graph, const ReportRequest& request);

// Drops whole rows until the rendered report fits `budget` tokens: lowest-weight
// dependencies first, then lowest-weight modules. Never trims a row. 0 = no budget.
void shed_to_budget(ModulesReport& report, ReportFormat format, std::size_t budget);

[[nodiscard]] nlohmann::json modules_report_json(const ModulesReport& report);
[[nodiscard]] std::string render_modules_mermaid(const ModulesReport& report);
[[nodiscard]] std::string render_modules_svg(const ModulesReport& report);
[[nodiscard]] std::string render_modules_markdown(const ModulesReport& report);
// The text an agent receives for `format`: the JSON payload dump or the rendered
// diagram. Budget accounting measures exactly this string.
[[nodiscard]] std::string render_modules_report(const ModulesReport& report, ReportFormat format);

// ~4 characters per token: the one estimate the report and context ops pack
// against. The length overload costs text that is not materialized yet.
[[nodiscard]] std::size_t estimate_report_tokens(std::string_view text);
[[nodiscard]] std::size_t estimate_report_tokens(std::size_t byte_length);

// The daemon `report` op: a full {ok, result|error} envelope. A reserved view
// answers ok:false with code "report_view_not_implemented" so a host can tell
// "not yet" from a malformed request.
[[nodiscard]] nlohmann::json report_response(const GraphSnapshot& graph, const nlohmann::json& params,
                                             const std::filesystem::path& project_root);

// Every daemon op is dispatched by name; a graphd built before this op answers
// "unknown op: report". Returns the upgrade hint for that case, nullopt otherwise.
[[nodiscard]] std::optional<std::string> report_upgrade_hint(const nlohmann::json& daemon_response);

}  // namespace cgraph
