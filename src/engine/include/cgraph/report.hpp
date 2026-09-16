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
// selector. `modules`, `types` and `clones` are implemented; `design` is
// reserved so the tool surface an agent discovers is stable while it lands
// (CGR-11).
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
inline constexpr std::size_t kDefaultMinMembers = 3;
inline constexpr std::size_t kDefaultMinTokens = 30;

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
  // Similarity floor shared by the types view (member-set Jaccard) and the
  // clones view (fingerprint Jaccard), and the clones view's token floor: a
  // body shorter than this is boilerplate (a getter, a main stub), not a clone.
  double threshold = 0.80;
  std::size_t min_tokens = kDefaultMinTokens;
  // types view: a type takes part in shape comparison only when it declares at
  // least this many members, so `{id, name}` pairs do not flood the report.
  std::size_t min_members = kDefaultMinMembers;
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

// ---- types view (CGR-9) ------------------------------------------------------
// A type is a `class` or `type` node; its members are the `field` nodes it
// `defines`, compared by label. Four findings, each a whole row: differently
// named types with exactly the same member set (identical, grouped), a label
// declared in several files (duplicates), pairs whose member sets nest or are
// at least `threshold` Jaccard-similar (overlaps), and types no other symbol or
// file in the graph refers to (unreferenced).
struct TypeRef {
  std::string id;
  std::string label;
  std::string kind;  // class | type
  std::string source_file;  // root-relative when under the project root
  std::size_t line = 0;
  std::vector<std::string> members;  // sorted, unique field labels
  std::size_t incoming = 0;          // non-structural edges into the type
};

struct IdenticalTypes {
  std::vector<std::string> shape;  // the member set every type in the group declares
  std::vector<TypeRef> types;      // by label, then file
};

struct DuplicateTypes {
  std::string label;
  std::vector<TypeRef> declarations;  // by source file
  // Lowest and highest pairwise member-set Jaccard among the declarations, so
  // the reader can tell one type copied into two files (1.0) from two unrelated
  // types that share a name (0.0). Declarations with no members compare as 0.
  double min_jaccard = 0.0;
  double max_jaccard = 0.0;
};

struct TypeOverlap {
  TypeRef a;  // for `subset`, the smaller type
  TypeRef b;
  std::size_t shared = 0;
  double jaccard = 0.0;
  std::string relation;  // subset | overlap
};

struct TypesReport {
  std::string scope;
  bool include_tests = false;
  double threshold = 0.80;
  std::size_t min_members = kDefaultMinMembers;
  std::vector<IdenticalTypes> identical;   // widest shape first, then largest group
  std::vector<DuplicateTypes> duplicates;  // most alike first (min_jaccard), then most declarations
  std::vector<TypeOverlap> overlaps;       // jaccard desc, shared desc
  std::vector<TypeRef> unreferenced;       // most members first
  std::size_t total_types = 0;
  std::size_t total_with_members = 0;
  std::size_t total_identical = 0;
  std::size_t total_duplicates = 0;
  std::size_t total_overlaps = 0;
  std::size_t total_unreferenced = 0;
  std::size_t omitted_identical = 0;
  std::size_t omitted_duplicates = 0;
  std::size_t omitted_overlaps = 0;
  std::size_t omitted_unreferenced = 0;
};

[[nodiscard]] TypesReport build_types_report(const GraphSnapshot& graph, const ReportRequest& request);
// Whole rows only, in value order: identical groups outrank duplicates, which
// outrank overlap pairs, which outrank unreferenced types; within a section the
// list is best-first, so the tail of the last populated section goes first.
void shed_to_budget(TypesReport& report, ReportFormat format, std::size_t budget);
[[nodiscard]] nlohmann::json types_report_json(const TypesReport& report);
[[nodiscard]] std::string render_types_markdown(const TypesReport& report);
// json or markdown; the types view has no diagram form, so mermaid and svg
// render as markdown here and `report_response` refuses them with a typed error.
[[nodiscard]] std::string render_types_report(const TypesReport& report, ReportFormat format);

// ---- clones view (CGR-10) ----------------------------------------------------
// Function bodies whose fingerprints (fingerprint.hpp) are at least `threshold`
// Jaccard-similar, grouped into classes by union-find. A class whose members
// all lie under test roots is a test class: reported, but after the production
// classes, because duplicated test fixtures are the commonest clone and the
// least urgent one.
struct CloneMember {
  std::string id;
  std::string label;
  std::string source_file;  // root-relative when under the project root
  std::size_t line = 0;
  std::size_t end_line = 0;
  std::uint32_t tokens = 0;
};

struct CloneClass {
  std::vector<CloneMember> members;  // by file, then line
  double similarity = 0.0;           // lowest pairwise Jaccard inside the class
  std::uint32_t tokens = 0;          // smallest member body, in normalized tokens
};

struct ClonesReport {
  std::string scope;
  bool include_tests = false;
  double threshold = 0.80;
  std::size_t min_tokens = kDefaultMinTokens;
  std::vector<CloneClass> classes;       // largest first, then most similar, then longest
  std::vector<CloneClass> test_classes;  // every member under a test root (empty when include_tests)
  std::size_t total_functions = 0;       // function nodes in scope
  std::size_t total_fingerprinted = 0;   // of those, with a fingerprint
  std::size_t total_eligible = 0;        // of those, at or above min_tokens
  std::size_t total_classes = 0;
  std::size_t total_test_classes = 0;
  std::size_t total_members = 0;         // functions in any class
  std::size_t omitted_classes = 0;
  std::size_t omitted_test_classes = 0;
  // Set when functions lack fingerprints: a graph fast-loaded from a persist
  // made before this build has none until the next rescan.
  std::string hint;
};

[[nodiscard]] ClonesReport build_clones_report(const GraphSnapshot& graph, const ReportRequest& request);
// Whole classes only: test classes shed first (smallest first), then
// production classes.
void shed_to_budget(ClonesReport& report, ReportFormat format, std::size_t budget);
[[nodiscard]] nlohmann::json clones_report_json(const ClonesReport& report);
[[nodiscard]] std::string render_clones_markdown(const ClonesReport& report);
// json or markdown; diagram formats render as markdown here and are refused by
// `report_response`, as for the types view.
[[nodiscard]] std::string render_clones_report(const ClonesReport& report, ReportFormat format);

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
