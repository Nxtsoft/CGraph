# Windows x64 port

## Why

The plugin launcher already maps win32 paths (`%LOCALAPPDATA%\cgraph`, `.exe`), but CGraph does
not build on Windows: the engine uses POSIX file descriptors, `stat` identity and `fsync`; the
daemon IPC is a Unix-domain socket whose Windows branches are stubs that print "not implemented";
the client cannot spawn a daemon; and `graphd` fails to link (`run_static_seam_server` is defined
only on POSIX). There is no Windows CI job and no `windows-x64` release asset. Item 4 of the build
plan; PR #181 lists what a port needs.

## What Changes

- **IPC transport: AF_UNIX on both platforms.** Winsock has supported `AF_UNIX` `SOCK_STREAM` at a
  filesystem path since Windows 10 1803 (build 17063); every supported Windows and the
  `windows-latest` runner have it. The daemon keeps one framing, serve-loop and client code path;
  every Winsock/BSD difference (handle type, `closesocket`, `WSAGetLastError`, `WSAPoll`,
  `ioctlsocket` non-blocking mode, `DWORD` millisecond timeouts, `SIGPIPE`, deleting the socket
  file) sits behind `local_socket.hpp`. Named pipes were the alternative; they would need a second
  transport (overlapped I/O, different accept and liveness semantics) behind the same frames.
- **Endpoint and identity.** Sockets live in `%LOCALAPPDATA%\graphd` (POSIX unchanged:
  `$XDG_RUNTIME_DIR/graphd` or a per-user temp dir). The directory gets a protected DACL granting
  the current user's SID alone, inherited by the socket file: the Windows form of mode 0700. The
  per-root hash reads the path as UTF-8 (POSIX bytes unchanged) and folds ASCII case on Windows,
  where `C:\Repo` and `c:\repo` are one directory.
- **Process seams (`platform.hpp`).** Process id, executable path (`GetModuleFileNameW`), the
  `.exe` suffix, `$HOME`/`%USERPROFILE%`, `$USER`/`%USERNAME%`, owner-only directories, detached
  spawn (`CreateProcessW` with no inherited handles, no console, a new process group; `fork` +
  `setsid` + `/dev/null` on POSIX), binary stdio for the MCP server, and Windows command-line
  quoting.
- **Filesystem.** `atomic_write` uses the CRT descriptor calls on Windows and replaces with
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, retried for up to 10 s while a reader holds the target
  open without `FILE_SHARE_DELETE` (Windows refuses the replace then; POSIX does not). There is no
  directory fsync on Windows. File identity and change tokens come from the volume serial number,
  file index and `ChangeTime`; `incremental_update` now reads identity through the watcher's token
  instead of its own `stat`.
- **Build and CI.** `windows-default` / `windows-release` presets (MSVC + Ninja, the existing
  `x64-windows-static-md` triplet), `/utf-8`, `ws2_32`/`advapi32`. CI gains both Windows presets
  and uploads the packaged `cgraph-windows-x64.tar.gz`; `release-binaries.yml` gains the
  `windows-x64` row. Both workflows package through `scripts/package_release.sh`.
- **Tests.** New `local_socket_test` and `platform_test`. Tests that needed only the socket now
  use the seam; tests that need a POSIX-only facility (RLIMIT_FSIZE, FIFOs, SIGSTOP, `wait4`) skip
  that case on Windows and print why. Test fixtures are written in binary mode so CRLF translation
  cannot change their bytes.

## Non-goals

- `cgraph daemon` supervision and `cgraph drain` on Windows (launchd/systemd only; both already exit
  with a message).
- Peer-credential checks on Windows (`SIO_AF_UNIX_GETPEERPID`); access is enforced by the
  directory DACL instead.
- Cutting a release or pinning a Windows asset in `integrations/launcher/release.json`; that waits
  for the next release run.
- No graph content changes, so the index version key is not bumped.

## Impact

- New: `src/engine/local_socket.cpp`, `src/engine/platform.cpp` (+ headers),
  `tests/smoke/local_socket_test.cpp`, `tests/smoke/platform_test.cpp`,
  `tests/smoke/unreadable_file.hpp`, `scripts/package_release.sh`.
- Changed: `daemon_server.cpp`, `daemon_endpoint.cpp` (named-pipe stubs removed),
  `daemon_lifecycle.cpp`, `daemon_identity.cpp`, `atomic_write.cpp`, `file_watcher.cpp`,
  `incremental_update.cpp`, `workspace.cpp`, `skills_install.cpp`, `client_runtime.cpp`,
  `src/cli/main.cpp`, `src/mcp/main.cpp`, CMake, presets, both workflows.
