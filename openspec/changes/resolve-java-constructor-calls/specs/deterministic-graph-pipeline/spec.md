## ADDED Requirements

### Requirement: Java constructor calls resolve to the constructed class
The system SHALL extract a Java `new Foo()` / `new Foo<T>()` / `new pkg.Foo()`
(`object_creation_expression`) as a call whose callee is the bare simple class name, so it resolves
project-wide to that class node — reconnecting a test to the classes it constructs (and, through the
class's `method`/`contains` edges, to their members). A `method_invocation` callee SHALL continue to
resolve by its `name` field, unchanged.

#### Scenario: A plain constructor call resolves to its class
- **GIVEN** a method that calls `new Widget()`
- **WHEN** extraction runs
- **THEN** the callee is `Widget`, and it resolves to the `Widget` class node

#### Scenario: Generic and qualified constructors reduce to the simple name
- **GIVEN** calls `new ArrayList<String>()` and `new java.util.HashMap<String, Integer>()`
- **WHEN** extraction runs
- **THEN** the callees are reduced to `ArrayList` and `HashMap` (not `ArrayList<String>` or `java.util.HashMap`)

#### Scenario: Method calls are unaffected
- **GIVEN** a plain method call `helper()`
- **WHEN** extraction runs
- **THEN** it still resolves by its `name` field exactly as before
