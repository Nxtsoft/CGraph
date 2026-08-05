## ADDED Requirements

### Requirement: An optimized build is provided and covered by CI
The project SHALL provide a `release` preset configuring an optimized build, and CI SHALL build and test it alongside `default` and `sanitizers`. Published performance numbers SHALL name the preset that produced them. An optimized build SHALL produce a `graph.json` byte-identical to the unoptimized build's, so optimization is never a correctness variable.

#### Scenario: CI covers the build users are told to make
- **WHEN** CI runs on a pull request
- **THEN** the matrix includes the `release` preset
- **AND** the documented build instructions name that same preset

#### Scenario: Optimization does not change the graph
- **WHEN** the same project is built with the `default` and `release` presets
- **THEN** the emitted `graph.json` is byte-identical between them
