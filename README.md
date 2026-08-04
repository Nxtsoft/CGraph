<div align="center">

<img src="assets/hero.svg" alt="CGraph — your codebase as a queryable knowledge graph" width="100%">

**English** · [简体中文](README.zh-CN.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-6ea8fe?style=flat-square)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-vcpkg-064F8C?style=flat-square&logo=cmake&logoColor=white)](CMakePresets.json)
[![MCP](https://img.shields.io/badge/MCP-2024--11--05-59d499?style=flat-square)](https://modelcontextprotocol.io)
[![PRs welcome](https://img.shields.io/badge/PRs-welcome-f5c451?style=flat-square)](#contributing)

*Scan a project · extract structure into a deterministic graph · query it in ~10 ms from a daemon, a thin client, or an MCP server.*

</div>

## Contents

[Why CGraph?](#why-cgraph) ·
[Architecture](#architecture) ·
[Repository layout](#repository-layout) ·
[What it produces](#what-it-produces) ·
[Output formats](#output-formats) ·
[Performance](#performance) ·
[Languages](#languages) ·
[Quick start](#quick-start) ·
[Install & Setup](#install--setup) ·
[Use with coding agents](#use-with-coding-agents) ·
[CLI · daemon · MCP reference](#cli-daemon-and-mcp-reference) ·
[Host integrations & enrichment](#host-integrations-and-enrichment) ·
[Development notes](#development-notes) ·
[Contributing](#contributing) ·
[License](#license)

## Why CGraph?

Reading a large repo to answer **"what calls this?"** or **"what breaks if I change it?"** means loading dozens of files — and those answers aren't in any single file. They live in the *relationships between* files. CGraph precomputes them so you (and your AI agent) navigate by **graph, not grep**.

| | |
| --- | --- |
| 🔗 **Reverse dependencies & blast radius** | `graph_impact` returns every transitive dependent of a symbol in ~10 ms. On a real 10,706-node repo it surfaced **93 dependents** of one god-file, instantly — an answer grep can't give you. |
| 🧭 **Navigate, don't grep** | Centrality-ranked search, node neighborhoods, shortest paths, and **token-budgeted source bundles** — served warm in ~10 ms instead of many file reads that burn context. |
| 🏛️ **Architecture at a glance** | Centrality ranking surfaces the load-bearing files; Leiden/Louvain community detection clusters the repo into modules automatically. |
| ⚡ **Deterministic & fast** | A full **10k-node graph builds in ~2 s**; no LLM in the extraction path — same input, same graph, every time. |
| 🤖 **Built for coding agents** | A standard **MCP** server drops the graph straight into Claude / Codex, so they reason over structure instead of blindly reading files. |

> **The pitch in one line:** for a codebase too big to hold in your head, CGraph is the index that decides *which* code to read — `ctags`/"find all references", precomputed, centrality-ranked, and exposed over MCP.

## Architecture

<div align="center"><img src="assets/architecture.svg" alt="CGraph architecture" width="100%"></div>

- **`cgraph`** — one-shot scan → portable disk exports (`graph.json`, `graph.html`, `graph.svg`, `obsidian.md`, `cypher.txt`, `call-flow.html`).
- **`graphd` + `cgraph-client`** — a resident per-project daemon with live file-watching; warm `query` / `path` / `explain` / `impact` / `context` in ~10 ms.
- **`cgraph-mcp`** — a Model Context Protocol server so agents navigate the graph directly.

The engine owns deterministic extraction, fragment validation, cache state, and local graph mutation; hosts own model choice and semantic enrichment.

## Repository layout

```text
src/cli/          One-shot CLI entrypoint: cgraph
src/daemon/       Daemon entrypoint: graphd
src/client/       Thin client runtime and cgraph-client executable
src/mcp/          MCP request handling and cgraph-mcp executable
src/engine/       Detection, extraction, graph building, analysis, daemon ops
tests/smoke/      CTest smoke coverage for engine, daemon, MCP, and integration paths
tests/fuzz/       Optional libFuzzer targets
integrations/     Host hook and always-on integration scripts
docs/             Host integration contract and benchmark notes
vendor/           Vendored tree-sitter core and grammars
```

## What it produces

A single scan turns a source tree into an interactive, explorable graph — communities colored, hubs sized by centrality:

<div align="center"><img src="assets/graph-example.png" alt="CGraph interactive viewer on a 10,708-node codebase" width="90%"></div>

<sub>The real `graph.html` viewer on a 10,708-node / 28,945-edge codebase — self-contained HTML, no external JS.</sub>

## Output formats

- `graph.json` — directed node-link JSON with graph metadata, nodes, and links
- `graph.html` — browser-readable interactive graph view
- `graph.svg` — static graph visualization
- `obsidian.md` — markdown export for Obsidian-style navigation
- `cypher.txt` — Neo4j Cypher statements
- `call-flow.html` — browser-readable call-flow view

## Performance

The interactive viewer used to run an O(N²) force simulation in the browser on every open. Layout is now **precomputed once in C++** (via igraph), and repaints are viewport-culled — both **dependency-free**:

<div align="center"><img src="assets/performance.svg" alt="Time-to-settle: 35s to 0ms" width="100%"></div>

| graph | time-to-settle **before** | **after** | per-frame repaint |
| --- | --- | --- | --- |
| 10,000 nodes | **35.2 s** (main thread pinned) | **~0 ms** (static layout) | **20–34× cheaper** (zoom/pan) |

## Languages

Tree-sitter-backed structural extraction, with regex/structured extraction for a few config formats:

<p>
<img src="https://img.shields.io/badge/C-A8B9CC?style=for-the-badge&logo=c&logoColor=black" alt="C">
<img src="https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++">
<img src="https://img.shields.io/badge/C%23-512BD4?style=for-the-badge&logo=dotnet&logoColor=white" alt="C#">
<img src="https://img.shields.io/badge/Go-00ADD8?style=for-the-badge&logo=go&logoColor=white" alt="Go">
<img src="https://img.shields.io/badge/Groovy-4298B8?style=for-the-badge&logo=apachegroovy&logoColor=white" alt="Groovy">
<img src="https://img.shields.io/badge/Java-007396?style=for-the-badge&logo=openjdk&logoColor=white" alt="Java">
<img src="https://img.shields.io/badge/JavaScript-F7DF1E?style=for-the-badge&logo=javascript&logoColor=black" alt="JavaScript">
<img src="https://img.shields.io/badge/Kotlin-7F52FF?style=for-the-badge&logo=kotlin&logoColor=white" alt="Kotlin">
<img src="https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python">
<img src="https://img.shields.io/badge/Ruby-CC342D?style=for-the-badge&logo=ruby&logoColor=white" alt="Ruby">
<img src="https://img.shields.io/badge/Scala-DC322F?style=for-the-badge&logo=scala&logoColor=white" alt="Scala">
<img src="https://img.shields.io/badge/TypeScript-3178C6?style=for-the-badge&logo=typescript&logoColor=white" alt="TypeScript">
<img src="https://img.shields.io/badge/TSX-61DAFB?style=for-the-badge&logo=react&logoColor=black" alt="TSX">
</p>

Plus structured/regex extraction for Apex, Delphi form/source, MSBuild/XML project files, and MCP config files.

## Quick start

**Prerequisites:** CMake 3.25+, Ninja, a C++20 compiler, Git, a Fortran compiler (`gfortran` — igraph pulls in `lapack-reference`), and vcpkg. See [Install & Setup](#install--setup) for the full recipe.

```sh
git clone --recurse-submodules https://github.com/taylor009/CGraph.git && cd CGraph
git clone https://github.com/microsoft/vcpkg .vcpkg && ./.vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD/.vcpkg"
cmake --preset release && cmake --build --preset release

# build a graph of this repo, then open the interactive viewer
build/release/src/cli/cgraph --root . --out cgraph-out
open cgraph-out/graph.html
```

## Install & Setup

> **Status:** early native implementation. The full command surface (CLI, daemon, thin client, MCP server) is present and tested; there is no packaged release yet — you build from source with CMake + vcpkg and run the binaries from the build tree (or symlink them onto your `PATH`).

<details>
<summary><strong>📋 Full build recipe — prerequisites · vcpkg · PATH · sanitizer &amp; fuzzer presets</strong></summary>

### Prerequisites

- CMake 3.25 or newer
- Ninja
- A C++20 compiler (recent Clang or GCC; Apple Clang from Xcode Command Line Tools works)
- A Fortran compiler (e.g. `gfortran`) — `igraph`'s vcpkg build pulls in `lapack-reference`, which needs one (`sudo apt-get install -y gfortran` / `brew install gcc`)
- Git
- vcpkg (a local copy is fine — see step 2). `curl`, `igraph`, `nlohmann-json`, and `utf8proc` are declared in `vcpkg.json` and built on first configure. `tree-sitter` is vendored under `vendor/tree-sitter`.

### 1. Clone (with submodules)

```sh
git clone --recurse-submodules https://github.com/taylor009/CGraph.git && cd CGraph
# already cloned without submodules?
git submodule update --init --recursive
```

### 2. Point CMake at vcpkg

```sh
git clone https://github.com/microsoft/vcpkg .vcpkg   # full depth — a shallow clone omits the pinned baseline
./.vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD/.vcpkg"
```

### 3. Configure, build, verify

```sh
cmake --preset release
cmake --build --preset release            # first build compiles vcpkg deps — several minutes
ctest --preset release                    # smoke suite
build/release/src/cli/cgraph --root . --out cgraph-out
```

Binaries land at `build/release/src/{cli/cgraph, daemon/graphd, client/cgraph-client, mcp/cgraph-mcp}`.

### 4. (Optional) Put binaries on PATH

```sh
mkdir -p ~/.local/bin
for b in cli/cgraph daemon/graphd client/cgraph-client mcp/cgraph-mcp; do
  ln -sf "$PWD/build/release/src/$b" ~/.local/bin/
done
```

> MCP client configs (below) should still use absolute paths to the binaries, since a client may not inherit your interactive shell's `PATH`.

### Development builds

```sh
cmake --preset default    && cmake --build --preset default    && ctest --preset default       # Debug, no -O
cmake --preset sanitizers && cmake --build --preset sanitizers && ctest --preset sanitizers  # ASan/UBSan
cmake --preset fuzzers    && cmake --build --preset fuzzers    && ctest --preset fuzzers      # libFuzzer
```

The fuzzer preset requires a Clang toolchain with the libFuzzer runtime; use an upstream LLVM/Clang toolchain if Apple Command Line Tools lack it.

</details>

## Use with coding agents

`cgraph-mcp` is a standard [MCP](https://modelcontextprotocol.io) server over stdio (protocol `2024-11-05`). Register it once and your agent navigates the codebase through fast graph queries instead of blind grep/read:

| Tool | Purpose |
| --- | --- |
| `graph_query` | Search nodes by text; ranked by centrality |
| `graph_explain` | A node's neighborhood (callers, callees, imports) |
| `graph_impact` | Transitive blast radius of changing a node |
| `graph_path` | Shortest path between two nodes |
| `graph_context` | Token-budgeted source bundle for a node/query (with adaptive gather) |
| `graph_update` | Content-verified sync; returns a `content_root` to pin reads |
| `graph_status` | Daemon, graph, and enrichment status |
| `graph_remember` / `graph_recall` | Session memory — checkpoint before `/compact`, recall after |
| `graph_shutdown` | Stop the daemon |

`graph_context` has two gather modes. The default (`gather: "fixed"`) packs the whole k-hop neighborhood. With a task query in hand, `gather: "adaptive"` keeps the full 2-hop core but expands the third hop only along query-relevant nodes — on the retrieval eval it lifted grade-2 recall **+0.057** for **+13%** candidate tokens, versus the **+96%** a full 3-hop gather costs (needs a `query`/`q`).

The server resolves the project root from `--root`, then `CLAUDE_PROJECT_DIR`, then the working directory, and finds `graphd` on its own (explicit `--daemon` wins, then `CGRAPH_DAEMON_PATH`, then a `graphd` next to `cgraph-mcp`). The first call triggers a one-time build (seconds); while it runs, results carry `"graph_state": "building"` so an empty result is never mistaken for "no match". Subsequent queries are warm (~10 ms). In the examples below, replace `/abs/path/to/CGraph` with this repo's absolute path.

<details>
<summary><strong>🔌 Register with Claude Code · Codex · Cursor / Windsurf / other MCP clients</strong></summary>

### Claude Code

Claude Code sets `CLAUDE_PROJECT_DIR` per session, so a single registration works across every project:

```sh
claude mcp add --scope user --transport stdio cgraph \
  -- /abs/path/to/CGraph/build/release/src/mcp/cgraph-mcp \
     --daemon /abs/path/to/CGraph/build/release/src/daemon/graphd
```

Or commit a project-scoped `.mcp.json` at the repo root to share it with collaborators:

```json
{
  "mcpServers": {
    "cgraph": {
      "command": "/abs/path/to/CGraph/build/release/src/mcp/cgraph-mcp",
      "args": ["--daemon", "/abs/path/to/CGraph/build/release/src/daemon/graphd"]
    }
  }
}
```

Verify with `/mcp` inside Claude Code. This repo also ships host skills under `integrations/skills/` — `cgraph` (reach for the graph first on structure questions) and `cgraph-enrich` (the semantic-enrichment loop). Install with `cgraph skills install`; add the scheduled enrichment drainer (status-gated) with `cgraph drain install`.

### Codex CLI

Codex does not set `CLAUDE_PROJECT_DIR`, so the server falls back to the working directory Codex launches it from:

```sh
codex mcp add cgraph \
  -- /abs/path/to/CGraph/build/release/src/mcp/cgraph-mcp \
     --daemon /abs/path/to/CGraph/build/release/src/daemon/graphd
```

…or edit `~/.codex/config.toml` directly (add `"--root", "/abs/path/to/your/project"` to `args` to pin a project regardless of working directory):

```toml
[mcp_servers.cgraph]
command = "/abs/path/to/CGraph/build/release/src/mcp/cgraph-mcp"
args = ["--daemon", "/abs/path/to/CGraph/build/release/src/daemon/graphd"]
```

Restart Codex and run `/mcp` in the TUI to confirm.

### Cursor, Windsurf, and other MCP clients

Any MCP client that launches a stdio command works — add a server entry with the command and args (most use a `mcpServers` JSON block like Claude Code's `.mcp.json`). Set `--root` explicitly for clients that don't set `CLAUDE_PROJECT_DIR`. Smoke-test the server by hand:

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  | build/release/src/mcp/cgraph-mcp --root . \
      --daemon build/release/src/daemon/graphd
```

</details>

## CLI, daemon and MCP reference

<details>
<summary><strong>⌨️ CLI · 🛰️ daemon &amp; thin client · 🔌 MCP server internals</strong></summary>

### CLI

```sh
cgraph [--root PATH] [--out PATH]
cgraph enrich-plan [--root PATH] [--out PATH] [--drop DIR]
cgraph enrich-ingest [--root PATH] [--out PATH] [--drop DIR]
```

Defaults: `--root .`, `--out cgraph-out`, `--drop` → CGraph's semantic drop directory under the output path.

```sh
# Build deterministic exports.
build/release/src/cli/cgraph --root /path/to/project --out /tmp/cgraph-out
# Create a semantic chunk plan for host enrichment.
build/release/src/cli/cgraph enrich-plan --root /path/to/project --out /tmp/cgraph-out
# Ingest host-written chunk_NN.json fragments and re-export the graph.
build/release/src/cli/cgraph enrich-ingest --root /path/to/project --out /tmp/cgraph-out
```

### Daemon & thin client

```sh
build/release/src/daemon/graphd --root /path/to/project
```

The daemon watches the project tree while it runs: source edits fold into the graph incrementally within a couple of seconds (a large batch, e.g. a branch switch, collapses into one full rescan), and incremental state re-persists to `cgraph-out/` in the background and on shutdown. `--no-watch` disables this.

Optional daemon flags:

```sh
graphd --root PATH --idle-timeout SECONDS --no-watch
graphd --benchmark-query --graph PATH --query TEXT
graphd --version
```

Use the thin client (responses are JSON; `status` includes process metadata, node/edge counts, cache hit rate, and enrichment state):

```sh
build/release/src/client/cgraph-client --root /path/to/project status
build/release/src/client/cgraph-client --root /path/to/project query '{"q":"Parser"}'
build/release/src/client/cgraph-client --root /path/to/project explain '{"id":"Parser"}'
build/release/src/client/cgraph-client --root /path/to/project path '{"source":"A","target":"B"}'
build/release/src/client/cgraph-client --root /path/to/project update '{"path":"."}'
build/release/src/client/cgraph-client --root /path/to/project shutdown
```

### MCP server

`cgraph-mcp` speaks MCP over stdio: newline-delimited JSON-RPC 2.0 implementing `initialize`, `tools/list`, `tools/call`, and `notifications/initialized` (protocol `2024-11-05`). Tool calls route through the same daemon operation handler used by the thin client; invalid JSON receives a JSON-RPC parse error. For registration and the tool list, see [Use with coding agents](#use-with-coding-agents).

</details>

## Host integrations and enrichment

<details>
<summary><strong>🧩 Host hook &amp; always-on loop · 🧠 semantic enrichment fragments</strong></summary>

### Host integrations

CGraph keeps provider and model concerns outside the native binary. Host integrations use `cgraph-client` for graph operations and dispatch semantic work through their own agent/model workflow. The reference hook accepts the deterministic daemon operations:

```sh
integrations/hooks/cgraph-hook.sh status
integrations/hooks/cgraph-hook.sh query '{"q":"GraphSnapshot"}'
```

Useful environment variables: `CGRAPH_CLIENT` (client executable), `CGRAPH_PROJECT_ROOT` (project root), `CGRAPH_DAEMON` (daemon path), `CGRAPH_INTERVAL_SECONDS` (always-on interval, default `30`), `CGRAPH_REFRESH_ON_START` (`0` to skip the initial update), `CGRAPH_ONCE` (`1` to run one status check and exit). Run the always-on reference loop:

```sh
CGRAPH_CLIENT=build/release/src/client/cgraph-client \
CGRAPH_PROJECT_ROOT=/path/to/project \
integrations/always-on/cgraph-always-on.sh
```

See `docs/host-skill-contract.md` for the full host contract.

### Semantic enrichment

A host-driven workflow: (1) CGraph emits a chunk plan for uncached or stale semantic inputs; (2) the host processes each chunk with its own model/agent; (3) the host writes exactly one `chunk_NN.json` fragment per completed chunk into the semantic drop directory; (4) CGraph validates each fragment before graph mutation; (5) valid fragments update the graph and semantic cache, malformed fragments are rejected without changing the snapshot.

Fragments use this node-link shape (required: node `id`/`label`; edge `source`/`target`/`relation`; hyperedge `id`/`nodes`/`relation`. Optional: `source_file`, `source_location`, `type`/`kind`, `confidence`, `confidence_score`, `properties`, `warnings`):

```json
{
  "nodes": [{ "id": "doc:architecture", "label": "Architecture", "kind": "document" }],
  "edges": [{ "source": "doc:architecture", "target": "component:engine", "relation": "describes" }],
  "hyperedges": []
}
```

</details>

## Development notes

- Keep extraction behavior deterministic in the engine. Provider-specific logic belongs in host integrations.
- Add smoke coverage under `tests/smoke/` for engine behavior and integration surfaces.
- Add fuzzer coverage under `tests/fuzz/` for parser or extractor hardening.
- Prefer extending the central language configuration and extractor pipeline over adding ad-hoc extraction logic in consumers.
- The interactive viewer is intentionally **dependency-free** — no JS libraries in the emitted HTML.

## Contributing

Issues and PRs welcome. Build with the `default` preset, keep `ctest --preset default` green, and prefer the `sanitizers` preset while iterating.

## License

[MIT](LICENSE).
