# reproducible-ci-dependencies Specification

## Purpose
TBD - created by archiving change reproducible-ci-dependencies. Update Purpose after archive.
## Requirements
### Requirement: Fresh checkouts materialize pinned parser sources
The repository SHALL declare every tree-sitter gitlink consumed by the native build as a submodule with its canonical upstream URL, and every CI checkout SHALL recursively materialize those submodules at the commits recorded by the parent repository.

#### Scenario: Normal matrix checkout
- **WHEN** a build-matrix job checks out a pull request or pushed commit
- **THEN** every tree-sitter path consumed by `vendor/tree-sitter/CMakeLists.txt` exists at the gitlink commit recorded by that parent commit

#### Scenario: Fuzzer checkout
- **WHEN** the fuzzer job checks out a pull request or pushed commit
- **THEN** it materializes the same complete tree-sitter dependency set before CMake configuration

### Requirement: CI uses one manifest-pinned native dependency graph
The repository SHALL select vcpkg port versions through the single `builtin-baseline` in `vcpkg.json`, and the existing Linux and macOS jobs MUST install that graph without platform-specific version overrides or fallback implementations.

#### Scenario: Supported build matrix
- **WHEN** GitHub Actions runs every declared operating-system and preset combination
- **THEN** dependency installation succeeds and each job configures, builds, and passes its CGraph test preset

#### Scenario: Fuzzer smoke job
- **WHEN** GitHub Actions runs the declared fuzzer job
- **THEN** dependency installation uses vcpkg's native toolchain, CGraph's fuzzer preset selects Clang, and the configured fuzz smoke tests build and pass

### Requirement: Dependency metadata is internally consistent
The repository SHALL keep submodule paths, upstream URLs, parent-index commits, and the documented tree-sitter pins consistent.

#### Scenario: Static dependency-contract validation
- **WHEN** the repository dependency metadata is inspected at a commit
- **THEN** each tree-sitter gitlink has exactly one matching `.gitmodules` entry and its recorded commit matches `vendor/tree-sitter/PINS.md`

### Requirement: An optimized build is provided and covered by CI
The project SHALL provide a `release` preset configuring an optimized build, and CI SHALL build and test it alongside `default` and `sanitizers`. Published performance numbers SHALL name the preset that produced them. An optimized build SHALL produce a `graph.json` byte-identical to the unoptimized build's, so optimization is never a correctness variable.

#### Scenario: CI covers the build users are told to make
- **WHEN** CI runs on a pull request
- **THEN** the matrix includes the `release` preset
- **AND** the documented build instructions name that same preset

#### Scenario: Optimization does not change the graph
- **WHEN** the same project is built with the `default` and `release` presets
- **THEN** the emitted `graph.json` is byte-identical between them

