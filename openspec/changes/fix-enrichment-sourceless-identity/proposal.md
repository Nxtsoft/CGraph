## Why

The dedup file-scoped guard keeps two enrichment (`document`/`media`) nodes from fuzzy-merging
across files, but keys the block on `source_file` values *differing*. Two document nodes that
BOTH omit `source_file` compare equal (`"" == ""`), so the guard does not fire and one is
silently deleted -- "escape-path B", the residual left open by `fix-enrichment-kind-identity`
(#26). `source_file` is host-optional per `integrations/skills/cgraph-enrich/SKILL.md`, so a
non-compliant host that drops it loses cross-file protection for its documents.

## What Changes

- In `semantic_dedup`'s fuzzy pass, keep a file-scoped enrichment pair apart unless they share
  the SAME non-empty `source_file`. The guard condition gains `|| left_node.source_file.empty()`:
  a differing file is kept apart (unchanged), a shared non-empty file may still merge (a genuine
  re-extraction of one document), and a shared EMPTY file is now kept apart (the fix).
- Rewrite the guard comment, whose stated rationale ("a document that omits its source_file keeps
  label-only identity") is exactly the behavior being corrected.
- Non-goals: no fragment-validation change (rejecting document/media without `source_file` would
  break the documented-optional contract and 9 existing test fragments across 4 files, and
  contradict `semantic_fragment_validation_test.cpp` which asserts a source-file-less document is
  valid); no change to the fuzzy thresholds, the kind set, or code-symbol/concept dedup.

## Impact

- Closes escape-path B and, as a bonus, the document(no-sf)-vs-concept(no-sf) merge (a document
  that dropped its file no longer collapses into a concept). Only pairs where a file-scoped kind
  is present AND both source_files are empty change behavior; every other pair is bit-identical.
- Over-block verified absent: two documents sharing one real `source_file` with different labels
  still merge (the `.empty()` clause preserves it); two concepts still merge by label (guard is
  kind-scoped); a document with a file vs a concept is unchanged.
- **Touches:** `src/engine/dedup.cpp` (one condition + comment), `tests/smoke/dedup_test.cpp`
  (escape-path B regression + a no-over-block same-file fixture).
- Residual (irreducible, acceptable): a node omitting BOTH kind and `source_file` stays
  label-mergeable -- indistinguishable from a concept, and combined with #26 (which stamps kind
  for sourced nodes) the realistic hole is closed. Blocking merges on "no source_file" alone would
  break the `PaymentService`/`Payment Service` Graphify parity and concept dedup.

## Capabilities

### Modified Capabilities

- `deterministic-graph-pipeline` -- file-scoped enrichment identity requires a real shared file.
