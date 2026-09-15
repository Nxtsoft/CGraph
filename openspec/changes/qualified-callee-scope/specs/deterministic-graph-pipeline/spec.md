## MODIFIED Requirements

### Requirement: Call sites are keyed on the callee name
A call site's callee key SHALL be the callee's name, resolved through the grammar: the leaf name reached by descending a qualified identifier's `name` field (which nests to arbitrary depth), a template function's or template method's `name`, and the property name of a member access.

A qualified callee SHALL also carry its qualifier: the scope text as written at the call site (`std` for `std::find(...)`, `proj::detail` for `proj::detail::helper()`), collected from every `qualified_identifier` on the way to the leaf. A C-family symbol declared inside one or more namespaces SHALL carry a `scope` property holding the `::`-joined enclosing namespace names, outermost first, with `(anonymous)` for an unnamed namespace; an inline namespace SHALL contribute no segment. A definition whose own declarator is qualified SHALL append that qualification (`int proj::Cache::reload() {}` carries `proj::Cache`). A symbol at file scope whose declarator is unqualified SHALL carry no `scope` property.

A qualifier segment SHALL be the scope's bare identifier: a class template scope `Outer<proj::Beast>` contributes `Outer`, and a scope that names no declaration -- `decltype(x)`, or a type parameter of an enclosing template -- SHALL void the whole qualifier. A qualifier rooted at a namespace alias SHALL be rewritten to the namespace that alias names. The qualifier and the `scope` property are compared as segment sequences, never by reducing text at a `::`.

Resolution order SHALL remain a symbol declared in the caller's own file, then a project-wide symbol whose name is unique, with the existing overload, import-narrowing and member-call tiers unchanged. When the call carries a qualifier, every candidate those tiers produce (the target and any overload siblings) SHALL be kept only if the qualifier's segments are a suffix of the candidate's `scope` segments with `(anonymous)` segments ignored, or the candidate is a method of a class whose label equals the qualifier's last segment and whose own `scope` carries the remaining segments. The qualifier SHALL be applied only where it can contradict a candidate: when its outermost segment is `std`, or when at least one candidate carries a `scope` property or belongs to a class. When no candidate carries either, the call SHALL resolve exactly as an unqualified call does. A set with no surviving candidate SHALL be refused and counted as `dropped_scope_mismatch`, and SHALL NOT be counted as `resolved_overload_first`. `CallResolution::balances()` SHALL include `dropped_scope_mismatch`.

A member call with an unknown receiver whose normalized name is one every standard library defines on its containers, strings, iterators, smart pointers and option types (`size`, `find`, `empty`, `begin`, `value`, `unwrap`, ...) SHALL NOT be bound by the method-only project-wide tier; when a project method of that name existed to refuse, the call is counted as `dropped_library_member`, otherwise it remains `dropped_unknown`. Same-file and receiver-named bindings are unaffected.

#### Scenario: A standard-library call does not bind to a same-named project function
- **GIVEN** `proj::find` is the only project function named `find`
- **AND** a function outside `proj` contains the call site `std::find(v.begin(), v.end(), 3)`
- **THEN** no `CALLS` edge to `proj::find` is emitted and `dropped_scope_mismatch` is incremented

#### Scenario: A nested standard-library call does not capture a same-file declaration
- **GIVEN** a file declares `exists` and contains the call site `std::filesystem::exists(p)`
- **THEN** no `CALLS` edge to that file's `exists` is emitted

#### Scenario: A namespace-qualified call resolves through its namespace
- **GIVEN** `helper` is declared inside `namespace proj { namespace detail { ... } }`
- **AND** another file contains the call site `proj::detail::helper()`
- **THEN** a `CALLS` edge to `helper` is emitted

#### Scenario: A class-qualified static call resolves through its class
- **GIVEN** `Stats` declares a method `size_of`
- **AND** a call site reads `proj::Stats::size_of()`
- **THEN** a `CALLS` edge to `size_of` is emitted

#### Scenario: A static call through a class template resolves through its class
- **GIVEN** `proj::Outer<T>` declares a static method `make`
- **AND** a call site reads `proj::Outer<proj::Beast>::make()`
- **THEN** a `CALLS` edge to `make` is emitted

#### Scenario: A qualified call reaches a symbol in an anonymous namespace
- **GIVEN** `hidden` is declared inside `namespace proj { namespace { ... } }`
- **AND** a call site reads `proj::hidden()`
- **THEN** a `CALLS` edge to `hidden` is emitted

#### Scenario: An overload set is gated as a whole
- **GIVEN** one file declares `dup` at file scope and `dup` inside `namespace alpha`, in that order
- **AND** another file contains the call site `alpha::dup(1.0)`
- **THEN** a `CALLS` edge is emitted to the `alpha` declaration only

#### Scenario: A different namespace root does not match on the innermost segment
- **GIVEN** `helper` carries scope `proj::detail`
- **AND** a call site reads `other::detail::helper()`
- **THEN** no `CALLS` edge is emitted and `dropped_scope_mismatch` is incremented

#### Scenario: A call to an out-of-line member definition resolves through the class it names
- **GIVEN** `int Cache::reload() { ... }` is defined inside `namespace proj`, with only a prototype in the class body
- **AND** another file contains the call site `proj::Cache::reload()`
- **THEN** a `CALLS` edge to `reload` is emitted

#### Scenario: A call through a namespace alias resolves to the aliased namespace
- **GIVEN** `helper_decl` is declared inside `namespace proj { namespace detail { ... } }`
- **AND** a file declaring `namespace pd = proj::detail;` contains the call site `pd::helper_decl()`
- **THEN** a `CALLS` edge to `helper_decl` is emitted

#### Scenario: An inline namespace is transparent to a qualified call
- **GIVEN** `versioned` is declared inside `namespace proj { inline namespace v1 { ... } }`
- **AND** another file contains the call site `proj::versioned()`
- **THEN** a `CALLS` edge to `versioned` is emitted

#### Scenario: A dependent call resolves on its leaf name
- **GIVEN** `template <typename T> int build() { return T::make(); }`
- **AND** the project declares exactly one `make`
- **THEN** a `CALLS` edge from `build` to `make` is emitted

#### Scenario: A qualified call binds a candidate that records no scope
- **GIVEN** `reload` is declared in a file with no `scope` property and no owning class
- **AND** another file contains the call site `proj::Cache::reload()`
- **THEN** a `CALLS` edge to `reload` is emitted
- **AND** the same project refuses `std::filesystem::remove(p)` against a project `remove` that records no scope either

#### Scenario: An unqualified call is unchanged
- **GIVEN** the same project
- **AND** a call site reads `find(3)`
- **THEN** resolution follows the existing same-file and unique-name rules

### Requirement: Call resolution is measurable from a committed artifact
`BuildStats` SHALL report, per build, `raw_calls_total` and a partition of it: `resolved_same_file`, `resolved_project_unique`, `resolved_member_method`, `dropped_unknown`, `dropped_ambiguous`, `dropped_self`, `dropped_scope_mismatch`, and `dropped_library_member`. The partition SHALL sum to `raw_calls_total`, and every field SHALL be serialized to `stats.json`. `resolved_overload_first` SHALL also be reported as a subset of the resolved fields.

#### Scenario: The resolution rate is readable without instrumenting a build
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/stats.json` contains every field
- **AND** the eight partition fields sum to `raw_calls_total`
