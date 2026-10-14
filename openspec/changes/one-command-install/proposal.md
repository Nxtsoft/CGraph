# One-command install

## Why

CGraph ships GitHub tarballs only: a user downloads, unpacks, symlinks and writes an MCP config by
hand, with absolute versioned paths (README Quick start). Competing code-graph tools install as a
Claude Code marketplace plugin or an `npx` package in one command. `cgraph daemon install` works
only on macOS (launchd), and the release builds no macOS x64 binary. CGR-27.

## What Changes

- The repository root becomes a Claude Code marketplace (`.claude-plugin/marketplace.json`) whose
  plugin is `integrations/`: the `cgraph` MCP server (`.mcp.json`), the `cgraph` and
  `cgraph-enrich` skills (`skills/`, unchanged) and the pre-edit hook (`hooks/hooks.json`).
- `integrations/launcher/cgraph-launch.mjs` runs any of the four executables from the release
  pinned in `integrations/launcher/release.json`, downloading the platform archive on first use,
  checking its SHA-256 and unpacking it by one atomic rename into the plugin data directory (or
  the user cache directory). No absolute paths are written anywhere.
- `server.json` describes `io.github.Nxtsoft/cgraph` for the official MCP registry with one npm
  package, `cgraph-mcp` (`packaging/npm/`), whose three bins wrap the same launcher. Nothing is
  published by this change.
- `cgraph daemon install|sync|status|uninstall` runs on Linux through systemd --user units with
  the same labels, flags and reconcile logic as the macOS LaunchAgents (`systemd_unit.cpp`).
- The release workflow builds `cgraph-macos-x64.tar.gz` on `macos-15-intel`;
  `scripts/pin_launcher_release.py` pins the launcher, plugin, npm package and server.json to a
  published tag.

## Non-goals

- Windows. The daemon IPC is a Unix-domain socket; the port is written up in the PR for a
  follow-up ticket.
- Homebrew formula, publishing to npm or the MCP registry, and `cgraph drain` on Linux.
- No graph content changes, so the index version key is not bumped.

## Impact

- New: `.claude-plugin/marketplace.json`, `integrations/.claude-plugin/plugin.json`,
  `integrations/.mcp.json`, `integrations/hooks/hooks.json`, `integrations/launcher/*`,
  `packaging/npm/*`, `server.json`, `scripts/pin_launcher_release.py`,
  `src/engine/systemd_unit.cpp`, `tests/smoke/systemd_unit_test.cpp`.
- Changed: `src/engine/daemon_supervisor.cpp`, `src/engine/launch_agent.cpp` (the command runner is
  shared with systemd), `src/cli/main.cpp`, `tests/smoke/daemon_supervisor_test.cpp`,
  `.github/workflows/{ci,release-binaries}.yml`, `README.md`.
