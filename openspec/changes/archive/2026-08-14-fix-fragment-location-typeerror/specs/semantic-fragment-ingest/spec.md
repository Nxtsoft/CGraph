## ADDED Requirements

### Requirement: Fragment parsing is total over any JSON shape
Parsing a semantic fragment SHALL NOT throw for any syntactically valid JSON input. Every
optional field, including each component of `source_location`, SHALL be read only when it holds
the expected type and SHALL degrade to a default otherwise, so a type-confused field can never
escape validation as an exception. A structurally valid fragment carrying garbage in an optional
field SHALL be accepted; only a structural violation (missing or mistyped `nodes`/`edges`,
missing required node/edge fields) SHALL reject it.

#### Scenario: A string-typed source_location line does not crash
- **GIVEN** a fragment whose node has `"source_location": {"start_line": "9"}` (a string where a number is expected)
- **WHEN** the fragment is validated
- **THEN** parsing returns without throwing and the fragment is accepted
- **AND** a resident daemon that ingests the dropped fragment stays alive and responsive

#### Scenario: Unreadable location components degrade, never fabricate a site
- **GIVEN** a `source_location` whose components are string, null, or bool
- **WHEN** the fragment is parsed
- **THEN** each unreadable component reads as 0, and a location object with no readable numeric component is treated as absent rather than a line-0/column-0 site

#### Scenario: Out-of-range location numbers do not wrap
- **GIVEN** a `source_location` with a negative or above-uint32 component
- **WHEN** the fragment is parsed
- **THEN** that component degrades to 0 rather than wrapping to a large or truncated line number
