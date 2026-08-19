---
name: cgraph-enrich
description: "Use to fill cgraph's semantic layer — connect documentation, design notes, and other prose to the deterministic code graph — when graph_status shows enrichment_pending > 0, or the user asks to enrich / index docs, build the knowledge graph over non-code files, or make documentation queryable alongside code. Drives the plan -> author fragment -> drop -> ingest loop: cgraph plans bounded chunks of uncached docs, you (the host model) author one validated fragment per chunk, and the daemon merges them. cgraph owns planning, validation, caching, and mutation; you own the semantic reading and authoring."
trigger: /cgraph-enrich
---

# cgraph-enrich

cgraph extracts code structure deterministically, but documentation, design
notes, and other prose only enter the graph as **host-authored semantic
fragments**. This skill is the loop that produces them. The native tool owns
chunk planning, fragment validation, content-hash caching, and graph mutation;
you (the model) own reading each chunk and authoring the fragment. No model
logic lives in the binary.

## The loop

Run from the project root. Repeat until the plan reports `0 input(s)`.

1. **Plan** — `cgraph enrich-plan --root . --out cgraph-out`
   Writes `cgraph-out/semantic-drop/plan.json`, listing only **uncached or
   stale** chunks. Each chunk has `inputs[].path` and a **`fragment`** field — the
   exact filename to drop. Already-enriched files are skipped via the content-hash
   cache. Fragment names are offset past the fragments already in the drop dir, so
   the directory **accumulates across passes** and a new pass never overwrites an
   earlier one — always read the fresh plan and use the names it gives.

2. **Author** — for each chunk in `plan.json`:
   - Read every file in `inputs[].path`.
   - Author ONE fragment capturing the chunk's concepts and how they relate to
     each other and to the code (see Fragment shape below).
   - Write it atomically to `cgraph-out/semantic-drop/<chunk.fragment>` — the
     filename from the chunk's `fragment` field, never a name you pick yourself.
     Write a temp file in that directory, then rename it into place. Do not reuse
     or overwrite an existing fragment file.

3. **Merge** — one of:
   - Batch: `cgraph enrich-ingest --root . --out cgraph-out` validates every
     dropped fragment, merges the valid ones, updates the cache, and re-exports.
   - Live: if a daemon is running (`graph_status`), dropping the file is enough —
     the watcher validates and merges it into the live snapshot within ~200ms.

4. **Verify & repeat** — re-run `enrich-plan`. Enriched files are now cache hits
   and drop out. `graph_status` shows `enrichment_pending` falling and
   `node_count` rising. Loop until pending is 0. Malformed fragments are rejected
   and leave the graph unchanged (`enrichment_state: failed`); fix and re-drop the
   same `chunk_<index>.json`.

## Fragment shape

Node-link JSON. Required: nodes need `id` + `label`; edges need `source` +
`target` + `relation`. Optional: `type`/`kind`, `source_file`, `source_location`,
`properties`, `confidence`, `hyperedges`.

```json
{
  "nodes": [
    {"id": "doc:architecture", "label": "Architecture", "type": "document",
     "source_file": "/abs/path/docs/architecture.md"},
    {"id": "concept:daemon-lifecycle", "label": "Daemon Lifecycle", "type": "concept"}
  ],
  "edges": [
    {"source": "doc:architecture", "target": "concept:daemon-lifecycle", "relation": "DESCRIBES"}
  ],
  "hyperedges": []
}
```

## Authoring guidance

- **Namespace ids** so semantic nodes never collide with code nodes: `doc:`,
  `concept:`, `topic:`. Set `source_file` (absolute path) on a doc node so it
  links back to where it came from.
- **Capture relationships, not just nodes.** A fragment whose only edges are
  `doc -> concept DESCRIBES` is thin. Add ordering (`PRECEDES`), composition
  (`PART_OF`), variants (`VARIANT_OF`), and cross-references between docs.
- **Connect prose to code.** When a doc is about a specific symbol or file, link
  it to that code node. Get the code node's real id from `graph_query` (or
  `graph_context`) first, then emit an edge `doc:... -> <code-node-id>` with a
  relation like `DESCRIBES` or `DOCUMENTS`. Do not invent or duplicate code
  nodes — reuse the ids the graph already has.
- **Reuse concept ids across chunks** so docs in different chunks attach to the
  same concept and the graph stays connected (e.g. every OpenSpec command doc
  points at one `concept:opsx-change-lifecycle`) — but **define every concept
  node locally in each fragment that references it**. An edge endpoint that is
  neither a node in the fragment nor an existing CODE node is rejected
  (`semantic fragment dependency has no source path`): the dependency
  fingerprinter cannot record a source-less external node, so cross-fragment
  concept reuse works by re-declaring the same `concept:` id (the merge
  deduplicates identical ids), never by bare reference.
- **Stay faithful.** The fragment is knowledge extracted from the files you read,
  not invention. If a file is low-signal (a license, a changelog), a single doc
  node with no edges is fine.

## Scope and idempotency

- The loop is safe to re-run and to run incrementally: the content-hash cache
  skips files already enriched (so you never redo work, and a changed file
  re-enters the plan as stale), and offset fragment names mean each pass adds to
  the drop dir rather than overwriting a prior pass. You can enrich a few
  high-value docs now and the rest later without losing the earlier work.
- For a large project, enrich the highest-value docs first (README, design docs,
  architecture, public API guides) — those connect the most code. Low-signal
  files can wait or be left out; the graph is useful incrementally.
- This skill never edits source and never adds provider/model logic to cgraph.
