#pragma once

#include "cgraph/types.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace cgraph {

// How many files co_change names by default: it runs before every edit, so the
// list stays short enough to read.
inline constexpr std::size_t kCoChangeDefaultLimit = 5;

// The files most likely to change together with an edit to one file, ranked,
// each with a one-line reason. `params`:
//   id     the edited file's node id (make_id of its root-relative path)
//   lines  [[first, last], ...] 1-based inclusive line ranges the edit touches;
//          absent or empty means the whole file (a Write)
//   limit  how many files to return (default kCoChangeDefaultLimit)
//
// The edited symbols are the innermost symbols of the file overlapping a range.
// Three ranked lists are interleaved, first unseen file from each in turn:
//   callers  files whose code reaches an edited symbol: directly (depth 1), then
//            through one intermediate symbol (depth 2); within a depth, more
//            edges first
//   tests    the file's source/test counterpart by name (`x.go` and `x_test.go`,
//            `foo.py` and `test_foo.py`, `a.ts` and `a.test.ts`/`a.spec.ts`,
//            `Foo.java` and `FooTest.java`), same directory first
//   linked   any other file joined by an edge to the file or one of its symbols,
//            in either direction, most edges first
// Answers {"id", "found", "edited": [labels], "files": [{"source_file", "why",
// "signal"}], "candidates": N}; `found` is false (and `files` empty) when no
// file node has that id. Reads the snapshot only: linear in nodes and edges.
[[nodiscard]] nlohmann::json co_change(const GraphSnapshot& graph, const nlohmann::json& params);

}  // namespace cgraph
