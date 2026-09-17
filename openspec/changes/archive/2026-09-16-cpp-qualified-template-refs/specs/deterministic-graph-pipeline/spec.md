## ADDED Requirements

### Requirement: Template arguments of qualified C++ types are references

When the C/C++ extractor collects the types referenced by a function signature or a data member, a namespace-qualified template type (`std::vector<T>`, `std::span<const T>`, `std::optional<T>`, `ns::Outer<T>`) SHALL be walked like an unqualified one: its base name SHALL be emitted as a reference candidate and each template argument SHALL be collected as a reference with `context: "generic_arg"`, recursively, so that a project type named only inside a qualified template's argument list receives a `references` edge when it resolves.

#### Scenario: A project type used only as a template argument is referenced
- **GIVEN** `struct Payload` in an included header and `std::optional<Payload> f(std::vector<Payload>& all, std::span<const Payload> view)`
- **WHEN** the file is extracted and relations are resolved
- **THEN** `f` has a `references` edge to `Payload` carrying `context: "generic_arg"`

#### Scenario: A qualified template data member is referenced
- **GIVEN** `struct Bag { std::vector<Payload> items; }`
- **THEN** `Bag` has a `references` edge to `Payload` with `context: "generic_arg"`
