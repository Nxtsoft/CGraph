# Tell the agent when a recalled checkpoint no longer matches the code

## Why

A checkpoint is a claim about code at the moment it was written. Today `remember` stores only
the touch key on each `concerns` edge (`daemon_ops.cpp`, `properties = {{"touch", touch}}`), so
`recall` presents every checkpoint as current, however far the code has moved since. When a
touched symbol is deleted, `rebind_memory_concerns` drops the edge during the next overlay, and
the link just disappears from recall with no trace.

The research run on 2026-09-26 (EA-Graph anchoring, arXiv:2608.04278, `interpretation`) measured
how often this happens on this repository. 90 commit-derived checkpoints written at 0cb8237 were
checked five weeks later at eb16b05. 77 of 90 (86%) touched at least one symbol whose body had
changed or been deleted, and name-only recall would have served 87 of the 90 as fully valid. 11 of
12 hand-labelled `changed` symbols were real semantic changes. The rows are commit-derived, so 86%
is an upper estimate. The failure is still the common case, not an edge case.

## What Changes

- `remember` anchors each resolved touch: it stores `anchor_sha256` on the `concerns` edge, the
  sha256 of the touched symbol's own source span. It is deliberately not
  `SnapshotSourceSnippet::source_sha256`, which hashes the whole file and would mark a checkpoint
  stale whenever anything else in the same file changed. The response reports `anchored`.
- `recall` grades every link `valid` / `changed` / `unanchored`, lists deleted touches under
  `gone`, and rolls these up into a checkpoint `validity` of `valid` / `stale` / `unverified`.
- The memory overlay records touches it cannot re-bind on the checkpoint node (`gone_touches`),
  recomputed on every overlay, so a deleted symbol is reported instead of silently lost.
- The MCP descriptions, the host skill and `docs/session-memory.md` explain the new fields.

The contract that tests verify:

- A checkpoint touching two functions in one file reports one `valid` and one `changed` after only
  the second function's body is edited.
- A function whose text is unchanged but shifted down the file stays `valid`.
- A touched function deleted by a rebuild appears under `gone` and makes the checkpoint `stale`.
  Overlaying again changes nothing. When the function returns with its original text, `gone` is
  empty and the link is `valid` again.
- A touch with no readable span, and every checkpoint written before this change, reads
  `unanchored`, never `valid`.

## Non-goals

- **Ranking.** Validity never filters or reorders recall, and memory stays inert to code retrieval
  (the "Memory nodes are inert to code analysis and retrieval" requirement is unchanged).
- **Diffs.** Recall reports that a span changed, not how. The agent reloads it with `graph_context`.
- **Executable validity conditions** ("true while `parse_config` reads the legacy key"). This is
  the larger idea in the research report. Content anchoring is its prerequisite and ships first.
- **Migration.** Existing sidecars are not rewritten. Their links read `unanchored` until the agent
  writes a new checkpoint.

## Impact

- `src/engine/daemon_ops.cpp`: `span_sha256`, `touch_validity`, `remember_checkpoint`,
  `recall_checkpoints`, `rebind_memory_concerns`, `overlay_memory_fragments`.
- `src/engine/include/cgraph/daemon_ops.hpp`: `rebind_memory_concerns` gains an optional
  `dropped_edges` out-parameter. Existing callers are unchanged.
- The sidecar format stays the same fragment schema. `anchor_sha256` is one more edge property,
  and `graph.json` still excludes memory.
- Recall responses gain `validity` and `gone` per checkpoint and `validity` per link. The fields
  are additive, and existing fields keep their shape.
