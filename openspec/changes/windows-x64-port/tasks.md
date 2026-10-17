# Tasks

- [x] `local_socket` seam: AF_UNIX over Winsock and BSD sockets; daemon server and client on it
- [x] Endpoint in `%LOCALAPPDATA%\graphd` with an owner-only protected DACL; UTF-8, case-folded identity hash
- [x] `platform` seam: process id, executable path, `.exe`, home/user, detached `CreateProcessW` spawn, binary stdio
- [x] `atomic_write`, `file_watcher`, `incremental_update` compile and behave on Windows
- [x] Windows presets, `/utf-8`, Winsock/advapi32 links; CI matrix and release row; shared packaging script
- [x] Smoke tests ported or skipped with a printed reason
- [ ] Next release run publishes `cgraph-windows-x64.tar.gz`; pin it in `integrations/launcher/release.json`
- [ ] Launcher E2E on Windows (install, `graph_query`) once the asset is published
