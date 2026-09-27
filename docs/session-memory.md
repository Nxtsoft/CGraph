# Session memory: checkpoint and recall

cgraph can act as a durable task-memory layer so a coding agent can `/compact` or `/clear`
a long session without losing the thread. The graph daemon (`graphd`) and `graph.json` are
**external to the agent's context window**, so a checkpoint written before `/clear` is still
there after it.

This is host/agent-authored memory. The engine stores it verbatim and keeps it inert to code
analysis and code retrieval — it never changes query/context/impact rankings.

## Tools

- `graph_remember {title, body, touches?, tags?}` — write one checkpoint. The `body` is a
  distilled markdown summary; `touches` lists code symbols (ids or names) the work concerns.
  The body is written under `cgraph-out/memory/` and the checkpoint node points at it.
- `graph_recall {query?, limit?}` — return recent checkpoints newest-first, each with its body
  summary and briefs of the code it touched, and whether that code has changed since (below).

## Workflow

```
before /compact or /clear ──► graph_remember(title, body, touches=[symbols])
after /clear ─────────────► graph_recall()            (restores the thread, ~KB payload)
                                  │
                                  └─► graph_context(id=<linked symbol>)   (reload code, budget-bounded)
after a long exploration ──► graph_remember(distilled finding)
before opening Playwright ─► graph_remember(what you're about to test)
```

The discipline is **distill → checkpoint → clear → recall**:

- Checkpoint a *distilled* summary — what you did and what's next.
- **Never** persist raw tool output, Playwright DOM snapshots, or chain-of-thought. Those are
  ephemeral; only the distilled outcome belongs in a checkpoint.
- After `/clear`, recall restores the task state cheaply; use `graph_context` on the linked
  symbols to reload just-enough code instead of re-reading files.

## Persistence (v1)

The primary use case works because **`/clear` does not stop the daemon** — it is a Claude Code
context operation. A checkpoint written before `/clear` is recall-able after it from the same
long-running daemon. Bodies live under `cgraph-out/memory/`; checkpoint nodes are added to the
live graph and persisted into `graph.json`.

Checkpoints also survive a daemon **restart**, incremental edits, and a full **rescan**: the
sidecar files under `cgraph-out/memory/` are the durable source of truth, and the daemon
re-overlays every checkpoint sidecar onto the graph after each rebuild (see
`daemon_server.cpp`, the memory re-ingest hook). Merging is first-occurrence-wins, so
re-applying an already-present checkpoint is a no-op.

## Validity: does the checkpoint still hold?

A checkpoint is a claim about code at the moment it was written, and code moves on. When
`remember` resolves a touch it stores `anchor_sha256` on the `concerns` edge: the sha256 of that
symbol's own source span (its lines, not the whole file). Spans are cut at the graph's line
numbers, so a symbol whose file was edited after the last extraction is not anchored (the watcher
re-extracts within a couple of seconds; `graph_update` forces it). `recall` re-hashes the live span
and grades every link:

| link `validity` | meaning |
|---|---|
| `valid` | the span hashes to the anchor; code that only shifted lines still counts |
| `changed` | the span differs from the anchor, can no longer be read, or its file was edited after the last extraction |
| `unanchored` | there is no anchor (written before anchoring existed, the symbol had no readable span, or its file had not been re-extracted yet) |

A touch whose symbol no longer exists is listed under the checkpoint's `gone`. The memory overlay
records these on the checkpoint node (`gone_touches`) when it cannot re-bind the edge after a
rebuild, instead of the link silently disappearing. The checkpoint's own `validity` is `stale` when
any link is `changed` or anything is `gone`, `unverified` when nothing is known to be stale but some
link cannot be proven (or there are no links), and `valid` otherwise.

Validity is information for the agent only. It never filters or reorders recall, and memory stays
inert to code ranking.
