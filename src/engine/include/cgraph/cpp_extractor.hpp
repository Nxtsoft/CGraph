#pragma once

#include "cgraph/extractor.hpp"

namespace cgraph {

// Relation/structure handlers shared by the C and C++ configs. They give the
// C-family extractor the same relation richness the JS/TS extractor already has:
//
//  - cpp_import_handler:   `#include` -> an `imports` edge (file -> included file),
//                          resolved to the real project file by resolve_imports;
//                          system/third-party includes resolve to nothing and are
//                          dropped (no dangling edges).
//  - cpp_relation_handler: base classes -> `inherits`, and member/parameter/return
//                          types -> `references` (resolved via includes).
//  - cpp_field_walk:       data members of a struct/class -> `field` nodes with a
//                          `defines` edge from the owning type.
//
// cpp_function_name is the `resolve_function_name` hook for the C family. A
// tree-sitter `function_definition` has no `name` field, so without it
// label_for_node falls through to `name_fields` and takes the declarator's raw
// text -- making a label the whole declaration (`run_one_shot(const
// std::filesystem::path& root)`, or 345 characters across 11 lines for a
// multi-line signature). Call sites record the bare callee identifier, so those
// two strings never matched and no C++ call to a function taking arguments
// resolved. It descends the declarator to the leaf identifier and reduces a
// qualified name to its tail, matching what Python, JavaScript, and TypeScript
// already produce. Returns empty for a construct it cannot name (a class, for
// instance), which leaves label_for_node's existing name-field path in charge.
void cpp_import_handler(const TSNode& node, const ExtractionContext& context, Fragment& fragment);
[[nodiscard]] std::string cpp_function_name(const TSNode& node, const ExtractionContext& context);
void cpp_relation_handler(const TSNode& node, const ExtractionContext& context, const std::string& node_id, std::vector<RawRelation>& out);
void cpp_field_walk(const TSNode& node, const ExtractionContext& context, Fragment& fragment, std::vector<RawCall>& raw_calls);

}  // namespace cgraph
