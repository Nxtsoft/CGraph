---
name: cgraph
description: "Code graph of this repo: definitions, callers, blast radius and focused source, faster than grep."
trigger: /cgraph
---

# cgraph

A live code graph of this project, served by a per-project daemon. One call
returns the exact file:line, and often the code itself, that would otherwise take
several grep and read rounds.

## Tools

- `graph_query {query, kind?, file?, limit?}`: find a symbol by name, or ask a
  structural question in plain words ("who calls X", "what does X call",
  "implementations of X"). The top three code hits carry their source `snippet`
  and `location`, so read the answer instead of opening the file. On zero matches
  it returns `suggestions`: fix the spelling and retry instead of grepping.
- `graph_context {query or id, budget}`: before an edit or review, the focal
  symbol's source plus its most relevant neighbors' source, packed to `budget`
  tokens (4000 is a good start). Read it instead of opening files one by one.
- `graph_impact {id, direction}`: `"dependents"` is what breaks if you change it,
  `"dependencies"` is what it relies on (transitive, bounded by `max_depth`).
- `graph_status`: whether the graph is built (`build_state`).

Id-taking tools also accept a bare symbol name. A name several symbols share comes
back `found: false, ambiguous: true` with the exact candidates: pick the `id` you
meant and call again.

## Gotchas

- The first call in a project builds the graph (seconds). If a result says
  `"graph_state": "building"`, an empty answer means "not built yet", not "no
  match": retry, or poll `graph_status` until `build_state` is `"ready"`.
- Edits land in the graph automatically within seconds. When you must rely on the
  graph matching the worktree exactly (after a batch of edits, or before impact
  analysis of moved code), call `graph_update {path: "."}` and pass its
  `freshness.content_root` as `expected_content_root` on the reads that follow.
- Use grep for string literals, comments, config values and file types cgraph
  does not extract. For symbols and how they connect, use the graph.

More tools (`graph_explain`, `graph_path`, `graph_report`, `graph_update`,
`graph_remember`, `graph_recall`) are listed when `CGRAPH_MCP_TOOLS=all` is set
before the session starts; `reference.md` in this skill's directory documents
them all, with every parameter.
