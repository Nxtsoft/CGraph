# Tasks

- [x] Marketplace + plugin manifest; MCP server and pre-edit hook through the launcher
- [x] Launcher with checksum verification and race-safe unpack; `node --test` suite in CI
- [x] Clean-HOME E2E on Linux: marketplace add, install, headless `claude -p` graph answer, timed
- [x] `server.json` validated against the 2025-12-11 schema; `npm pack` tarball run with `npx` in a clean HOME
- [x] systemd --user backend for `cgraph daemon`; unit tests incl. `systemd-analyze verify`
- [x] Linux E2E: install, live daemon, uninstall with no units left
- [x] Release matrix gains macos-x64; actionlint clean
- [ ] Publish npm package and registry entry (owner decision)
- [ ] Windows x64 port (follow-up ticket)
