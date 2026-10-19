## Why

The pre-edit hook (CGR-30) asks only a daemon that is already running and never starts one. In
the billed-cost A/Bs the agent never called a CGraph tool on its own, so no daemon existed in any
of 309 plugin runs (`graphd_stopped` was 0 in every one) and the hook never fired. Starting the
daemon at session start fixes that, but the daemon writes `cgraph-out/` into the repository, which
`git status` then shows as untracked in every repository opened.

## What Changes

- `ensure_output_dir_ignored` (`index_persistence.cpp`): the daemon creates `cgraph-out/` with a
  `.gitignore` holding `*` when it starts, and never rewrites an existing one.
- `should_prewarm` (`mcp_server.cpp`): `cgraph-mcp` starts the daemon at startup for a git
  repository root (`.git` dir or worktree file) or a root with `cgraph-out/`; other roots, such
  as a home directory, still wait for a tool call.

## Impact

- The pre-edit hook has a daemon to ask in any repository the plugin opens.
- Measured by re-running the multi-file A/B: the share of runs whose patch misses a gold source
  file (72.4% for baseline on the 70 tasks with two or more gold `.py` files).
