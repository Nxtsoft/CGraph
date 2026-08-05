## Why

`semantic_dedup`'s fuzzy pass merges distinct enrichment `document` nodes, deleting real content from the graph. Reproduced by compiling the committed `semantic_dedup` into a harness and running it over this repo's real enriched graph (`cgraph-out/graph.json`, 1773 nodes / 3327 links):

```
nodes 1773 -> 1728   (44 document nodes deleted)
edges 3327 -> 3183   (144 edges lost)
```

The pairs that collapse are different documents about the same subject:

| merged away | into | Jaro-Winkler |
| --- | --- | --- |
| `persist-incremental-index/proposal.md` | `persist-incremental-index/tasks.md` | 0.9435 |
| `plan-candidate-code-links/proposal.md` | `plan-candidate-code-links/design.md` | 0.9203 |
| `plan-candidate-code-links/tasks.md` | `plan-candidate-code-links/design.md` | 0.9346 |
| `commands/opsx-bulk-archive.md` | `commands/opsx-archive.md` | 0.9322 |

A change's proposal, its tasks, and its design are three different documents being merged into one node, and 144 edges pointing at them are rewritten or lost.

**This is pre-existing, not introduced by `fix-cpp-call-resolution`.** The same harness against that change's parent commit deletes the same 44 documents. It is filed separately because the fix needs a design decision that change should not make silently.

## Why the existing guards miss it

`fix-cpp-call-resolution` added two fuzzy-pass guards, and neither can fire here:

- The identical-label guard only applies to *identical* labels; these are merely similar.
- The declaration-site guard requires **both** nodes to have a `source_location`. Enrichment documents have none.

The shape is specific and was mis-stated in that change's first draft: `document` and `media` nodes **do** carry a `source_file` (measured 230/230 and 4/4 on this graph) but **no** `source_location`. `integrations/skills/cgraph-enrich/SKILL.md` instructs hosts to set the source file so a doc links back to where it came from. Only `concept` carries neither field. So documents take the from-a-file branch, which protects identical labels but not similar ones.

## What Changes

- Stop the fuzzy pass merging two enrichment nodes that came from **different source files**. A document's identity is the file it was written from, the same way a `file` node's identity is its path.
- Decide, explicitly, whether enrichment kinds (`document`, `media`, `concept`) should participate in cross-file fuzzy merging at all. Code symbols must keep doing so: `tests/smoke/dedup_test.cpp` asserts `PaymentService` in `a.cpp` merges with `Payment Service` in `b.cpp`, which is Graphify parity behaviour. Enrichment nodes are not obviously the same case.
- Tighten the fragment contract if that decision requires it. `fragment_json.cpp` parses `source_file` and `source_location` independently and validation imposes nothing, so all four presence combinations are reachable for any kind, and `"source_location": 42` degrades silently to absent.
- Add a regression test over two similar document labels in different files -- the suite has documents with a `source_file` (`semantic_ingest_test.cpp`, `host_surface_integration_test.cpp`) but never runs fuzzy dedup across two similar ones, which is why this was invisible.
- Non-goals: changing the Jaro-Winkler thresholds, changing code-symbol dedup, regenerating the retrieval fixture.

## Impact

- **Touches:** `src/engine/dedup.cpp`, `tests/smoke/dedup_test.cpp`, possibly `src/engine/fragment_json.cpp` and `semantic_fragment_validation.cpp`.
- **Recovers real content**: 44 document nodes and 144 edges per build on this repo. Any graph with enrichment is affected in proportion to how many similarly-named documents it has -- an OpenSpec-shaped repo, where every change has a `proposal.md`, `tasks.md` and `design.md`, is close to worst case.
- Fixing it changes `graph.json` for any enriched project, so the retrieval fixture's frozen shape drifts further from what the engine produces. That is already recorded in `fix-cpp-call-resolution`.

## Capabilities

### Modified Capabilities

- `deterministic-graph-pipeline` — enrichment-node identity in semantic dedup.
