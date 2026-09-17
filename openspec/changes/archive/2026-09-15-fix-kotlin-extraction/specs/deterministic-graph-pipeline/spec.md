## ADDED Requirements

### Requirement: Kotlin source files are extracted through the configured tree-sitter path
The system SHALL detect `.kt` / `.kts` files as Kotlin and extract, via the grammar-driven
configured extractor, class and object nodes, function nodes, and call edges — with the same
Graphify-compatible fragment shape as other configured languages — despite the `tree-sitter-kotlin`
grammar exposing no named fields on its declarations or `call_expression`. A name SHALL be resolved
positionally (a class/object by its `type_identifier` child, a function by its `simple_identifier`
child). A callee SHALL be reduced to its bare leaf name (a `recv.member()` navigation call to the
member name, a `Name()` call to the identifier) and kept eligible for project-wide resolution.

#### Scenario: Kotlin classes, objects, and functions become nodes
- **GIVEN** a `.kt` file declaring a `class`, an `object`, an `interface`, and named functions
- **WHEN** the one-shot pipeline runs
- **THEN** the class, object, and interface are `class` nodes, the functions are `function` nodes, and each type `contains`/owns its members

#### Scenario: Kotlin calls become edges
- **GIVEN** a function that calls a local function `helper()` and a navigation call `Registry.lookup()`
- **WHEN** extraction runs
- **THEN** a `CALLS` edge is produced for `helper`, and the navigation call is reduced to the bare name `lookup` and resolved by name (not left as the whole call's text)

#### Scenario: A previously empty Kotlin graph now carries symbols and edges
- **GIVEN** a Kotlin project that extracted zero nodes and zero edges before this change
- **WHEN** the one-shot pipeline runs
- **THEN** its classes, functions, and call edges appear in `graph.json`
