## MODIFIED Requirements

### Requirement: Call sites are keyed on the callee name
A call site's callee key SHALL be the callee's name, resolved through the grammar: the leaf name reached by descending a qualified identifier's `name` field (which nests to arbitrary depth), a template function's or template method's `name`, and the property name of a member access.

A qualified callee SHALL also carry its qualifier: the scope text as written at the call site (`std` for `std::find(...)`, `proj::detail` for `proj::detail::helper()`), collected from every `qualified_identifier` on the way to the leaf. A C-family symbol declared inside one or more namespaces SHALL carry a `scope` property holding the `::`-joined enclosing namespace names, outermost first, with `(anonymous)` for an unnamed namespace; a symbol at file scope SHALL carry no `scope` property.

Resolution order SHALL remain a symbol declared in the caller's own file, then a project-wide symbol whose name is unique, with the existing overload, import-narrowing and member-call tiers unchanged. When the call carries a qualifier, the declaration those tiers select SHALL be emitted only if its `scope` ends with the qualifier's innermost segment as a whole namespace name, or it is a method of a class whose label equals that segment. Any other selection SHALL be refused and counted as `dropped_scope_mismatch`, and `CallResolution::balances()` SHALL include that count.

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

#### Scenario: An unqualified call is unchanged
- **GIVEN** the same project
- **AND** a call site reads `find(3)`
- **THEN** resolution follows the existing same-file and unique-name rules
