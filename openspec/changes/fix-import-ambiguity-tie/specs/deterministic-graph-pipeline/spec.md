## MODIFIED Requirements

### Requirement: Call sites are keyed on the callee name
A call site's callee key SHALL be the callee's name, resolved through the grammar: the leaf name reached by descending a qualified identifier's `name` field (which nests to arbitrary depth), a template function's or template method's `name`, and the property name of a member access.

Resolution SHALL NOT be performed by reducing the callee's text at a scope separator. `::` legitimately appears in nine distinct callee node types, so no text rule distinguishes them: `ns::make<zoo::Beast>` reduced at its last `::` yields `Beast>`, which normalizes to `Beast` and fabricates a call to an unrelated struct while losing the real one.

A callee that names explicitly global scope (a qualified identifier with no scope, `::stat(...)`) SHALL resolve to nothing: it names a platform symbol, not a project one. Resolution order SHALL remain a symbol declared in the caller's own file, then a project-wide symbol whose name is unique. The exactly-one-candidate rule SHALL continue to govern the project-wide tier. A member call SHALL remain scoped to the caller's own file, because the receiver type is unknown. Confidence grading is unchanged: `EXTRACTED` when the caller's file imports the resolved symbol or its module, `INFERRED` when it is only a name match.

When the project-wide tier finds several candidates and they are not a single-file overload set, the candidate set SHALL first be narrowed to those the caller's file imports **by name** — the target of an `imports` or `re_exports` edge — and the ordinary rule re-applied to the survivors. Exactly one survivor SHALL resolve, graded `EXTRACTED`, because the import names the declaration outright. Several survivors sharing one file SHALL resolve as an overload set. Any other outcome SHALL remain `dropped_ambiguous`. A module-level `imports_from` edge SHALL NOT narrow the set: it names a file, not a declaration, and the call it would justify is spelled as a member access this tier never sees.

#### Scenario: A cross-file call to a parameterized function resolves
- **GIVEN** `graph_builder.cpp` declares `merge_fragments`
- **AND** `pipeline.cpp` contains the call site `merge_fragments(fragments)`
- **THEN** a `CALLS` edge exists from the enclosing symbol in `pipeline.cpp` to the `merge_fragments` node

#### Scenario: An import names which of two same-named declarations a call meant
- **GIVEN** `storage.py` and `cache.py` each declare `write_text`
- **AND** `report.py` has an `imports` edge to `storage.py`'s `write_text`
- **WHEN** a call to `write_text` in `report.py` is resolved
- **THEN** a `CALLS` edge to `storage.py`'s `write_text` is emitted with `EXTRACTED` confidence
- **AND** no `CALLS` edge to `cache.py`'s `write_text` is emitted
- **AND** `dropped_ambiguous` is not incremented

#### Scenario: Without an import the same call stays ambiguous
- **GIVEN** `storage.py` and `cache.py` each declare `write_text`
- **AND** `audit.py` imports neither
- **WHEN** a call to `write_text` in `audit.py` is resolved
- **THEN** no `CALLS` edge is emitted and `dropped_ambiguous` is incremented

#### Scenario: Importing both declarations of a name picks neither
- **GIVEN** a file has `imports` edges to two declarations that share a name and live in different files
- **WHEN** a call to that name is resolved
- **THEN** no `CALLS` edge is emitted and `dropped_ambiguous` is incremented

#### Scenario: An imported overload set edges to every member
- **GIVEN** two declarations of `add` in `Sum.java` and an unrelated `add` in `Other.java`
- **AND** the caller's file imports both `Sum.java` declarations
- **WHEN** a call to `add` is resolved
- **THEN** a `CALLS` edge is emitted to each `Sum.java` declaration
- **AND** `resolved_overload_first` is incremented
- **AND** no `CALLS` edge to `Other.java`'s `add` is emitted

#### Scenario: A module import alone does not break a tie
- **GIVEN** two files declare `write_text`
- **AND** the caller's file has only an `imports_from` edge to one of those files
- **WHEN** a bare call to `write_text` is resolved
- **THEN** no `CALLS` edge is emitted and `dropped_ambiguous` is incremented
