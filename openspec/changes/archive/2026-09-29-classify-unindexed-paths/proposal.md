# Report why a file is absent from the graph, not just that it is

## Why

Detection drops files at three points, each with a bare `continue`
(`src/engine/detect.cpp:198-216`): a dependency or gitignored **directory**
(recursion disabled), a gitignored **file**, and a file no language claims. The verdict is
control flow. Nothing records it.

A consumer therefore cannot tell these apart:

- `research/evidence/result.json` — deliberately ignored; `.gitignore:23` is `/research/`. The
  graph is complete without it.
- `CMakeLists.txt` — visited, not ignored, and unextracted. The graph may be **incomplete**
  because of it.

On cgraph PR #73, blastline emitted ~900 identical `has no graph node` reasons and fell back to
the full suite. All 896 were the first case — files cgraph had **already decided** to skip
(`git ls-tree -r --name-only origin/main research/ | wc -l` = 896, matching the reason count).
cgraph computed the right answer 896 times and discarded it.

Today no interface can answer the question:

- `describe_miss` (`daemon_ops.cpp:640-647`) returns identical JSON for ignored asset,
  unsupported language, extraction failure, and typo.
- `GraphSnapshot` (`types.hpp:101-118`) has no warnings field; real extraction failures written
  at `file_extraction.cpp:71,74` drain into a throwaway `PipelineResult` at `pipeline.cpp:58`.
- `configured_extractors.cpp:1372` **explicitly excludes** `Unknown` from `unextracted_counts`,
  so `status` could never have reported it.

## What Changes

- Add `classify_project_paths`, which walks the tree with the **same predicates and the same
  order** as `detect_project_files` and records the verdict at each skip point rather than
  dropping it.
- Write the result as a new `paths.json` sidecar in the export directory, alongside the nine
  sidecars already written (`graph.html`, `modules.mmd`, `design.md`, …).
- `graph.json` is **byte-identical**. The verdict is deliberately not a graph field: `graph.json`
  is a Graphify parity surface, and this is consumer metadata, not graph topology.

### The distinction that makes this worth doing

Two verdicts, and they are not interchangeable:

| verdict | meaning | may a consumer skip work over it? |
|---|---|---|
| `ignored` | gitignore match or dependency directory | **yes** — the graph is complete without it |
| `unindexed` | visited, not ignored, unextracted | **no** — the graph may be incomplete |

`CMakeLists.txt`, `.sh`, `.cmake` and `.yaml` all land in `unindexed`, and each can change what
a test means. **Collapsing the two would be worse than reporting neither**: it would invite a
consumer to skip work over a build file it cannot see. This proposal therefore reports them as
separate sets and never merges them.

### Non-goals

- Extracting any new language, or making unindexed files disappear.
- Any change to `graph.json`, node identity, edge semantics, or the content root.
- Telling a consumer what to do with the verdict.

## Impact

- New engine source and its 1:1 test; one additional exported file.
- Ignored **directories** are recorded by subtree root, never enumerated — an ignored
  `node_modules` is one entry, not a hundred thousand.
- No daemon op or MCP tool in this change; the export is the interface the current consumer
  (blastline, which reads the export directory from disk) actually uses.
