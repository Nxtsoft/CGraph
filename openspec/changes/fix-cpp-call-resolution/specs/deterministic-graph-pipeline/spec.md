## ADDED Requirements

### Requirement: Symbol labels name the symbol
A function, method, or field node's `label` SHALL be the symbol's name, not its declaration text. For C and C++ the name SHALL be the leaf identifier reached by descending the declarator through pointer, reference, array, and parenthesized wrappers, reduced to its tail for a qualified name. A label SHALL NOT contain a parameter list, a return type, or a newline. This aligns C and C++ with Python, JavaScript, and TypeScript, which already emit bare names.

#### Scenario: A C++ function label is its name
- **WHEN** the pipeline extracts `GraphSnapshot merge_fragments(std::span<const Fragment> fragments)`
- **THEN** the node's label is exactly `merge_fragments`

#### Scenario: Declarator decoration does not leak into the label
- **WHEN** the pipeline extracts a function returning a reference, a destructor, and an operator overload
- **THEN** the labels contain no leading `&` or `*`, and are the plain names `~Foo` and `operator==`

#### Scenario: No symbol is lost to an unnameable declarator
- **WHEN** a declaration's declarator yields no leaf identifier
- **THEN** the existing name-field path still supplies a label and no node is dropped

### Requirement: Call sites are keyed on the callee name
A call site's callee key SHALL be the callee's name, normalized the same way as a declaration label: the tail of a qualified identifier, and the property name of a member access. Resolution order SHALL remain a symbol declared in the caller's own file, then a project-wide symbol whose name is unique. The exactly-one-candidate rule SHALL continue to govern the project-wide tier. A member call SHALL remain scoped to the caller's own file, because the receiver type is unknown. Confidence grading is unchanged: `EXTRACTED` when the caller's file imports the resolved symbol or its module, `INFERRED` when it is only a name match.

#### Scenario: A cross-file call to a parameterized function resolves
- **GIVEN** `graph_builder.cpp` declares `merge_fragments`
- **AND** `pipeline.cpp` contains the call site `merge_fragments(fragments)`
- **THEN** a `CALLS` edge exists from the enclosing symbol in `pipeline.cpp` to the `merge_fragments` node

#### Scenario: A qualified call resolves
- **WHEN** `cli/main.cpp` contains `cgraph::run_one_shot(args.root)`
- **THEN** a `CALLS` edge exists to the `run_one_shot` node

#### Scenario: A C++ member call resolves within its own file
- **GIVEN** a struct declares a method `is_live`
- **AND** another function in the same file calls `handle->is_live()`
- **THEN** a `CALLS` edge exists to that method node
- **AND** the call is never matched project-wide

#### Scenario: An overloaded name resolves to nothing and is counted
- **GIVEN** two declarations in scope share the name `write_text`
- **WHEN** a call to `write_text` is resolved
- **THEN** no `CALLS` edge is emitted and `dropped_ambiguous` is incremented

### Requirement: A call target must be callable
Project-wide call resolution SHALL only consider candidates whose kind can be invoked: `function` and `class`. `class` remains eligible because `Foo()` is a constructor call in Python and JavaScript. A `field` node SHALL NOT be the target of a `CALLS` edge.

#### Scenario: A syscall name colliding with a struct field emits no edge
- **GIVEN** a struct declares a field named `connect`
- **AND** a function invokes the platform `::connect` symbol
- **THEN** no `CALLS` edge is emitted to the field node
- **AND** the call is counted in `dropped_unknown`

### Requirement: A namespace is not a class
A C++ `namespace_definition` SHALL NOT produce a `class` node. Containment from a namespace to its members SHALL use the `contains` relation, never `method`. A namespace node SHALL be excluded from degree-centrality computation and god-node ranking, on the same footing as session-memory nodes.

#### Scenario: Namespace membership does not dominate a path answer
- **GIVEN** two functions in different files, both inside `namespace cgraph`
- **AND** a real call chain connects them
- **WHEN** `path` is asked for a route between them
- **THEN** the returned path follows the call chain rather than consisting solely of shared namespace containment

#### Scenario: A namespace is never the highest-centrality node
- **WHEN** centrality is computed over a C++ project
- **THEN** no namespace node is ranked a god node

#### Scenario: Namespace containment is not a method edge
- **WHEN** a namespace contains a free function
- **THEN** the containment edge relation is `contains`

### Requirement: Call resolution is measurable from a committed artifact
`BuildStats` SHALL report, per build, `raw_calls_total` and a partition of it: `resolved_same_file`, `resolved_project_unique`, `dropped_unknown`, `dropped_ambiguous`, and `dropped_self`. The partition SHALL sum to `raw_calls_total`, and every field SHALL be serialized to `stats.json`.

#### Scenario: The resolution rate is readable without instrumenting a build
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/stats.json` contains every field
- **AND** the five partition fields sum to `raw_calls_total`
