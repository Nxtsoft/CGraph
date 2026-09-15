## ADDED Requirements

### Requirement: Type definitions expose declared members

When member extraction is enabled for a language, the extractor SHALL emit a field node for each supported directly declared member and a defines edge from its named owning class/type. Field IDs SHALL use `make_id(source_file + ":" + TypeName + "::" + member)` and source locations SHALL refer to the declaration. Declared type text SHALL be preserved when available. Optional and readonly grammar flags SHALL be string properties where applicable.

#### Scenario: Identical shapes have independent member lists
- **WHEN** two TypeScript interfaces, Go structs or Rust structs have different names and identical member declarations
- **THEN** both owners have complete identical member-label sets with distinct owner-qualified field IDs.

#### Scenario: Nested fields remain nested
- **WHEN** a TypeScript field has an inline object type or a class contains a nested class
- **THEN** nested declarations are not flattened into the outer owner's member list.

#### Scenario: Language-specific member shapes
- **WHEN** a Go declaration names multiple fields, a Rust tuple struct has positional fields, or Java declares multiple variables or record components
- **THEN** each declaration yields all its members with their declared type text.

#### Scenario: Python methods have local variables
- **WHEN** a class has class-body assignments and method-local assignments
- **THEN** only class-body declarations become members; local variables do not.

#### Scenario: Members never overwrite a declaration's own node
- **WHEN** a declaration inside a type body is already a function or type node (a TypeScript `method_signature`, a Rust trait `function_signature_item`, a Rust `type_item`)
- **THEN** no field node is emitted for it, and its `interface_method` tag and dispatch edges survive.

#### Scenario: Extraction remains opt-in
- **WHEN** member extraction is disabled
- **THEN** the new handler emits no nodes or edges and existing graph.json output remains byte-identical.

### Requirement: C/C++ forward declarations are not definitions

A named class, struct, union or enum specifier without a body SHALL NOT emit a class node. A later body-bearing definition SHALL emit the normal class node and members.

#### Scenario: FileCacheEntry forward declaration
- **WHEN** a source tree contains a forward declaration and one body-bearing FileCacheEntry definition
- **THEN** the graph contains exactly one FileCacheEntry class node.
