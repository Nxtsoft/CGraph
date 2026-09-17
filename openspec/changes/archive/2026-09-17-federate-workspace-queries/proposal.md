# Feature: workspace federation across per-repo daemons (CGR-14)

## Why

CGR-13 gave two repositories the same node for a contract: `endpoint:GET /api/v1/notebooks/{}/notes`
exists in the graph of the service that serves it and of every service that calls it, because the
id carries no repository. `seam discover` proves the join offline, by reading two `graph.json`
files and writing a fused artifact that goes stale on the next edit. What an agent actually asks
is live: "what breaks for the frontend if I change this handler?" Today that needs two calls
against two roots and a human to join them, and `seam fuse` needs rerunning after every change.

## What Changes

- **A workspace is a directory with `cgraph.workspace.json`** naming member repos:
  `{"name": "turing", "repos": [{"name": "api", "root": "./turing-api"}, …]}`. Roots under the
  workspace are stored relative so a checkout can move. `cgraph workspace init` writes it,
  discovering every git repository one level down when no `--repo NAME=PATH` is given;
  `cgraph workspace status` prints each repo's daemon state, node and edge counts, and the
  workspace totals.
- **Federation at one point** (`workspace.cpp`, new; entered from `send_thin_client_request`).
  When a request's root holds the manifest, the op is answered by asking each member repo exactly
  as a lone project is asked: same hooks, same auto-spawn, one daemon per root. So the thin
  client, the CLI and the MCP server all federate without a new tool, a new transport or a new
  resident process. No graph is copied anywhere; each repo keeps its own watcher and incremental
  updates, so an edit on either side is live on the next query.
- **`impact` crosses the contract.** Every repo is asked about the seed; the repos that have it
  are the owners. Any `endpoint:` node the traversal reached (or the seed itself, when it is one)
  is forwarded once to every repo with the depth that remains, and the returned witnesses are
  merged at the contract's depth plus their own, tagged with the repo they came from and the
  contract they crossed. One hop per contract, never a join over copied graphs.
- **`path` joins at a contract.** Both ends in one repo delegates to it. Otherwise the endpoints
  the source reaches (one `impact` call, nearest first, at most eight) are tried as bridges: a
  path from the source to the contract in its repo, and from the contract to the target in
  another, concatenated at the contract, which both name identically.
- **`query`, `explain`, `status`, `update` merge.** Query sums totals and re-ranks the merged hits
  by centrality, the only signal comparable across repos; explain answers from the repo that has
  the node and names the others sharing it (`also_in`); status reports per repo plus totals;
  update fans out. Every returned node carries `repo`.
- **A repo that cannot be reached is reported, never dropped.** Every federated result carries
  `unreachable: [{repo, root, error}]` when a daemon is down or answers with an error, so a
  partial answer can never look total.
- **Per-repo ops are refused loudly.** `report`, `context`, `remember`, `recall` and `shutdown`
  return `workspace_op_unsupported` naming each repo root: a report's layers and a context's
  token budget belong to one project, and merging them would invent something the caller did not
  ask for.

### Non-goals
- **A separate workspace daemon process.** The plan sketched one holding "only the contract
  layer". Everything it would hold is already in the per-repo daemons, so a proxy would add a
  process, an identity and a lifecycle for no capability; federation is a library both existing
  clients call. The property the plan wanted from it, per-repo incremental updates with one hop
  per cross-repo edge, is exactly what this does.
- Repo-scoped node ids (`repo:<name>`): separate, behind a flag with regenerated goldens. Two
  clones of one repository in a workspace would collide today, which is why `workspace init`
  names each repo and every result carries `repo`.
- Federated `report` and `context`: see above.
- Nested workspaces, and a member repo that is itself a workspace.
- Parallel fan-out: repos are asked in manifest order. Two repos answer in ~20 ms warm; a
  workspace large enough to need concurrency is a later change with a measurement behind it.

## Impact

- **Touches:** `src/engine/include/cgraph/workspace.hpp` and `src/engine/workspace.cpp` (new),
  `src/engine/CMakeLists.txt`, `src/client/client_runtime.cpp`, `src/cli/main.cpp`,
  `tests/smoke/workspace_test.cpp` (new), `tests/smoke/CMakeLists.txt`, docs.
- **No graph change.** A project root behaves exactly as before; only a root holding the manifest
  federates. `graph.json`, extraction and every export are untouched.
- **Measured** on a workspace over turing-api and turing-webapp (tasks.md 3.x).

## Capabilities

### New Capabilities
- `workspace-federation` — one read surface over several repositories, joined at their shared contract nodes.
