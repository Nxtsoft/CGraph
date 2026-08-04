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
A call site's callee key SHALL be the callee's name, resolved through the grammar: the leaf name reached by descending a qualified identifier's `name` field (which nests to arbitrary depth), a template function's or template method's `name`, and the property name of a member access.

Resolution SHALL NOT be performed by reducing the callee's text at a scope separator. `::` legitimately appears in nine distinct callee node types, so no text rule distinguishes them: `ns::make<zoo::Beast>` reduced at its last `::` yields `Beast>`, which normalizes to `Beast` and fabricates a call to an unrelated struct while losing the real one.

A callee that names explicitly global scope (a qualified identifier with no scope, `::stat(...)`) SHALL resolve to nothing: it names a platform symbol, not a project one. Resolution order SHALL remain a symbol declared in the caller's own file, then a project-wide symbol whose name is unique. The exactly-one-candidate rule SHALL continue to govern the project-wide tier. A member call SHALL remain scoped to the caller's own file, because the receiver type is unknown. Confidence grading is unchanged: `EXTRACTED` when the caller's file imports the resolved symbol or its module, `INFERRED` when it is only a name match.

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

#### Scenario: A template argument is never mistaken for the callee
- **GIVEN** calls `wrapper<zoo::Beast>(1)` and `ns::made<zoo::Beast>(2)`
- **THEN** no `CALLS` edge to `Beast` is emitted
- **AND** edges to `wrapper` and `made` are emitted

#### Scenario: An explicitly global callee resolves to nothing
- **WHEN** a function calls `::stat_local_probe(p)` and a local symbol of that name exists
- **THEN** no `CALLS` edge to the local symbol is emitted

#### Scenario: A same-file overload set resolves to its first declaration
- **GIVEN** two declarations in one file share the name `add`
- **WHEN** a call to `add` in that file is resolved
- **THEN** a `CALLS` edge to the first declaration is emitted with `INFERRED` confidence
- **AND** `resolved_overload_first` is incremented

#### Scenario: A project-wide ambiguous name resolves to nothing and is counted
- **GIVEN** two files each declare `write_text` and neither is the caller's file
- **WHEN** a call to `write_text` is resolved
- **THEN** no `CALLS` edge is emitted and `dropped_ambiguous` is incremented

#### Scenario: Overloads sharing one line remain distinct nodes
- **GIVEN** three declarations of `triple` on a single line
- **THEN** the graph holds three distinct `triple` nodes

### Requirement: A call target must be callable
Project-wide call resolution SHALL only consider candidates whose kind can be invoked, and that set SHALL be the same one the per-file table admits: `function`, `class`, `type`, and `variable`. `class` is eligible because `Foo()` is a constructor call in Python and JavaScript, and `type`/`variable` because a module-level binding can hold a callable. A `field` node SHALL NOT be the target of a `CALLS` edge.

The two resolution tiers SHALL agree on what counts as a symbol. The defect being corrected is that the project-wide tier applied no kind filter at all, not that it applied a different one.

#### Scenario: A syscall name colliding with a struct field emits no edge
- **GIVEN** a struct declares a field named `connect`
- **AND** a function invokes the platform `::connect` symbol
- **THEN** no `CALLS` edge is emitted to the field node
- **AND** the call is counted in `dropped_unknown`

### Requirement: A namespace is not a class
A C++ `namespace_definition` SHALL NOT produce a node of kind `class`, and SHALL NOT parent its members. Its members attach to the enclosing scope -- in practice the file -- so a namespace never contributes a `method` edge.

Ids are per-file, so a namespace-as-class minted one node per file all bearing the same label, grouping nothing across files while making every member read as a method of a type.

#### Scenario: A namespace produces no class node
- **WHEN** the pipeline extracts a file containing `namespace demo { int helper(int x); }`
- **THEN** no node of kind `class` labelled `demo` exists

#### Scenario: A namespace member attaches to its file
- **WHEN** a namespace contains a free function
- **THEN** the function has an incoming `contains` edge from its file node
- **AND** it has no incoming `method` edge

#### Scenario: A real class still owns its methods
- **WHEN** a class declares a method inline
- **THEN** the containment edge relation is still `method`

#### Scenario: No symbol is lost when the namespace node disappears
- **WHEN** a project's namespaces stop producing nodes
- **THEN** the count of function, class, field, import and variable nodes is unchanged
- **AND** no member is left without an incoming containment edge

### Requirement: Call resolution is measurable from a committed artifact
`BuildStats` SHALL report, per build, `raw_calls_total` and a partition of it: `resolved_same_file`, `resolved_project_unique`, `dropped_unknown`, `dropped_ambiguous`, and `dropped_self`. The partition SHALL sum to `raw_calls_total`, and every field SHALL be serialized to `stats.json`. `resolved_overload_first` SHALL also be reported as a subset of `resolved_same_file`.

#### Scenario: The resolution rate is readable without instrumenting a build
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/stats.json` contains every field
- **AND** the five partition fields sum to `raw_calls_total`
