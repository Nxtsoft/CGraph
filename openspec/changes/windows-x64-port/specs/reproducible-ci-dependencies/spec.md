## ADDED Requirements

### Requirement: Windows x64 is built and tested in CI
The project SHALL provide `windows-default` and `windows-release` presets (MSVC, Ninja, the
`x64-windows-static-md` vcpkg triplet), and CI SHALL build both on `windows-latest` and run the smoke
suite. A smoke case that needs a POSIX-only facility MAY skip on Windows only by printing the
reason; a whole test that cannot run SHALL exit with its declared `SKIP_RETURN_CODE`. The release
workflow SHALL build a `cgraph-windows-x64.tar.gz` holding `cgraph.exe`, `graphd.exe`,
`cgraph-client.exe` and `cgraph-mcp.exe`, packaged by the same script as every other platform.

#### Scenario: Pull request covers Windows
- **WHEN** CI runs on a pull request
- **THEN** the matrix builds and tests `windows-default` and `windows-release` on `windows-latest`
- **AND** the `windows-release` job uploads the packaged `cgraph-windows-x64.tar.gz`

#### Scenario: A skipped case says why
- **WHEN** a smoke case cannot run on Windows
- **THEN** its output names the case and the missing facility
