## ADDED Requirements

### Requirement: Rust source files are extracted through the configured tree-sitter path
The system SHALL detect `.rs` files as Rust and extract, via the grammar-driven configured
extractor, function nodes (free functions, impl methods, and trait method signatures), type nodes
(struct, enum, union, trait, and type alias), and call edges — with the same Graphify-compatible
fragment shape as other configured languages. A `Type::method()` or turbofish `foo::<T>()` callee
SHALL be reduced to its leaf name; an `x.method()` call SHALL be recorded as a same-file member
call.

#### Scenario: Rust structs and functions become nodes
- **GIVEN** a `.rs` file declaring a `struct`, an `impl` method, and a free function
- **WHEN** the one-shot pipeline runs
- **THEN** the struct is a `type` node, the impl method and free function are `function` nodes, and the file `contains` them

#### Scenario: Rust calls become edges, including same-file member calls
- **GIVEN** an impl method that calls a free function and `self.other_method()`
- **WHEN** extraction runs
- **THEN** a `CALLS` edge to the free function is produced, and the `self.method()` call resolves to the same-file method

#### Scenario: A qualified call is reduced to its name
- **GIVEN** a call `Type::make()`
- **WHEN** extraction runs
- **THEN** the callee is reduced to `make` (not dropped) and kept eligible for resolution
