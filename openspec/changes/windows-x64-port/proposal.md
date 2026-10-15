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
  instead of its own `stat`. The token also carries size and mtime: timestamps advance in ticks, and
  two writes inside one change-time tick were invisible to a ctime-only token (`file_watcher_test`
  failed that way on NTFS in CI and on a Linux 6.12 tmpfs).
- **Deterministic dedup across standard libraries.** The MinHash signatures that pick fuzzy-merge
  candidates hashed shingles with `std::hash<std::string>`, which is implementation-defined: MSVC
  uses FNV-1a, libc++ CityHash, libstdc++ a MurmurHash2 variant, so the three platforms merged
  different nodes (on Windows `dedup_test`'s `PaymentService` / `Payment Service` pair was never
  compared). `minhash_shingle_hash` spells out libstdc++'s function, so Linux output is unchanged
  and macOS and Windows now match it. macOS graphs can change, so the index key is bumped
  (`logic-24` -> `logic-25`).
- **Build and CI.** `windows-default` / `windows-release` presets (MSVC + Ninja, the existing
  `x64-windows-static-md` triplet), `/utf-8`, `ws2_32`/`advapi32`. CI gains both Windows presets
  and uploads the packaged `cgraph-windows-x64.tar.gz`; `release-binaries.yml` gains the
  `windows-x64` row. Both workflows package through `scripts/package_release.sh`.
- **Stack.** MSVC links every executable with an 8 MB stack reserve (`/STACK`), Linux's main-thread
  default; Windows' 1 MB overflowed the extractor's bounded AST walk in Debug.
- **Parity gate.** `pack_context_parity_test` measured with absolute paths in every context entry, so
  its result moved with the checkout root's length: main's own gate fails at budget 2000 from an
  18-character root, which is what `windows-latest` checks out to. It now serves context with
  `state.project_root` set, as `retrieval_quality_test` does (CGR-33), and is re-pinned. Measured
  root-independently, knapsack trails greedy at 2000 by 0.0361, outside the 0.03 band; that band is
  exempt pending a packer decision, both non-regression floors stay.
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

## Impact

- New: `src/engine/local_socket.cpp`, `src/engine/platform.cpp` (+ headers),
  `tests/smoke/local_socket_test.cpp`, `tests/smoke/platform_test.cpp`,
  `tests/smoke/unreadable_file.hpp`, `scripts/package_release.sh`.
- Changed: `dedup.cpp` (portable MinHash hash), `index_persistence.cpp` (key bump),
  `enrich_drainer.cpp`, `daemon_server.cpp`, `daemon_endpoint.cpp` (named-pipe stubs removed),
  `daemon_lifecycle.cpp`, `daemon_identity.cpp`, `atomic_write.cpp`, `file_watcher.cpp`,
  `incremental_update.cpp`, `workspace.cpp`, `skills_install.cpp`, `client_runtime.cpp`,
  `src/cli/main.cpp`, `src/mcp/main.cpp`, CMake, presets, both workflows.
