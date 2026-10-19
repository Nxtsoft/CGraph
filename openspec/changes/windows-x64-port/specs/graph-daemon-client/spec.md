## MODIFIED Requirements

### Requirement: Cross-platform local IPC
The daemon SHALL support length-prefixed JSON request and response frames over `AF_UNIX`
`SOCK_STREAM` sockets at a per-project filesystem path on Linux, macOS and Windows (Winsock
`AF_UNIX`, Windows 10 1803 and later). One framing, serve-loop and client code path SHALL serve all
three platforms, with platform differences confined to the local-socket seam. The thin client SHALL
auto-spawn a missing daemon on every platform, detached from the caller's stdio and console.

#### Scenario: Command frame succeeds
- **WHEN** a client sends a valid `query`, `path`, `explain`, `update`, `status`, or `shutdown` frame
- **THEN** the daemon processes the operation and returns a valid response frame

#### Scenario: Protocol version mismatch recovers
- **WHEN** a client connects with an incompatible protocol version
- **THEN** the daemon is shut down or rejected according to the version-skew policy and the client can respawn a compatible daemon

#### Scenario: Windows client reaches a live daemon
- **WHEN** a Windows thin client sends a frame to a project whose daemon is listening
- **THEN** it connects to `%LOCALAPPDATA%\graphd\graphd-<hash>.sock` and receives the response frame

#### Scenario: Stale socket file
- **WHEN** a daemon crashed and left its socket file behind
- **THEN** a liveness probe reports it dead, and the next daemon removes the file and binds

### Requirement: Secure daemon endpoint
The daemon SHALL restrict socket access to the current user and reject cross-user access where
platform support exists. The socket directory SHALL be owner-only: mode 0700 on POSIX; on Windows a
protected DACL granting only the current user's SID, inherited by the socket file.

#### Scenario: Unauthorized peer is rejected
- **WHEN** a peer from another user attempts to connect on a platform with peer credential support
- **THEN** the daemon rejects the connection before processing any graph command

#### Scenario: Windows socket directory is owner-only
- **WHEN** the daemon prepares its socket directory on Windows
- **THEN** the directory's DACL is protected and every ACE grants the current user's SID alone
