#pragma once

#include "cgraph/extractor.hpp"
#include "cgraph/operation_stats.hpp"
#include "cgraph/tsconfig_aliases.hpp"
#include "cgraph/types.hpp"

#include <span>

namespace cgraph {

[[nodiscard]] GraphSnapshot merge_fragments(std::span<const Fragment> fragments);
void merge_fragment(GraphSnapshot& graph, const Fragment& fragment);

// Canonical node identity used by merge: an explicit `id` if present, otherwise
// a normalized hash of source_file:kind:label. Exposed (not changing merge
// behavior) so a caller can compute exactly the ids merge will assign — e.g. the
// semantic-ingest referential-integrity check, which must match how merge keys
// nodes before deciding whether an edge endpoint exists.
[[nodiscard]] std::string node_key(const Node& node);

// Relinks import/module stub nodes (kind "import"/"module", carrying an
// `import_path` property) onto the real project file and declared symbol they
// refer to, then drops the now-redundant stubs. Imports to files outside the
// graph (e.g. third-party packages) are left as stubs. Run after merge_fragments
// and before resolve_raw_calls so call resolution can use a file's resolved
// imports.
void resolve_imports(GraphSnapshot& graph, std::span<const PathAlias> aliases = {});

// Resolves raw call sites into CALLS edges. When `outcomes` is non-null it
// receives a per-outcome tally of every call site seen, so the resolution rate is
// readable from a committed artifact instead of having to be inferred from the
// edges that happened to survive.
void resolve_raw_calls(GraphSnapshot& graph, std::span<const RawCall> raw_calls,
                       CallResolution* outcomes = nullptr);

// Resolves type/heritage facts (inherits/implements/references) onto real graph
// edges. Each target type name is resolved against the source file's imports
// and, for heritage relations only, a same-file declaration — never a
// project-wide name guess. Unresolvable targets emit no edge. Run after
// resolve_imports so a file's imports point at their real symbols.
// Interface-dispatch resolution (Go): computes `implements` edges from
// per-type method sets vs interface method sets, `dispatches_to` edges from
// each interface method to its implementations, and rescues member calls whose
// bare name was ambiguous among concrete methods but uniquely names an
// interface method — the `r.Match(...)` pattern that name-based resolution
// alone cannot bind. Runs after resolve_raw_relations (needs `method_of`).
void resolve_interface_dispatch(GraphSnapshot& graph, std::span<const RawCall> raw_calls);

void resolve_raw_relations(GraphSnapshot& graph, std::span<const RawRelation> raw_relations);

}  // namespace cgraph
