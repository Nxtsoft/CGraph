## MODIFIED Requirements

### Requirement: Rust source files are extracted through the configured tree-sitter path
The system SHALL detect `.rs` files as Rust and extract, via the grammar-driven configured
extractor, function nodes (free functions, impl methods, and trait method signatures), type nodes
(struct, enum, union, trait, and type alias), and call edges — with the same Graphify-compatible
fragment shape as other configured languages. A `Type::method()` or turbofish `foo::<T>()` callee
SHALL be reduced to its leaf name; an `x.method()` call SHALL be recorded as a same-file member
call.

Additionally, Rust `use` declarations SHALL resolve to project files through the module layout:
`::` paths map to `<path>.rs` or `<path>/mod.rs`, leading `crate::`/`self::`/`super::` segments
are stripped before unique-suffix matching, grouped/aliased/glob forms are expanded per leaf
(an alias resolves via the original name and never becomes a node), and an imported item SHALL
remap to its declared node in the resolved file when one exists (the file node otherwise). A
path resolving to no project file, or to more than one candidate file, SHALL leave no stub node
and no dangling edge in the graph.

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

#### Scenario: A plain item import resolves to the declared symbol
- **GIVEN** `src/a.rs` containing `use crate::foo::bar::Baz;` and `src/foo/bar.rs` declaring `Baz`
- **WHEN** the one-shot pipeline runs
- **THEN** the graph contains an `imports` edge from `src/a.rs`'s file node to `Baz`'s node

#### Scenario: The mod.rs layout resolves
- **GIVEN** the module lives at `src/foo/bar/mod.rs` instead of `src/foo/bar.rs`
- **WHEN** the same import resolves
- **THEN** it resolves to that file's declared symbol (or its file node when the item is not declared)

#### Scenario: An external crate import leaves the graph unchanged
- **GIVEN** a file containing `use serde::Serialize;` and no project file matching the path
- **WHEN** the one-shot pipeline runs
- **THEN** no import stub node and no edge referencing it remains

#### Scenario: Grouped and aliased imports expand per leaf
- **GIVEN** a file containing `use crate::foo::{bar::Baz, qux as Q};`
- **WHEN** extraction and resolution run
- **THEN** one resolved edge exists per leaf, and `qux as Q` resolves via the original name `qux`

#### Scenario: A glob names the module itself
- **GIVEN** a file containing `use crate::foo::bar::*;`
- **WHEN** the one-shot pipeline runs
- **THEN** the graph contains an `imports` edge from the importing file to `foo/bar`'s module file
