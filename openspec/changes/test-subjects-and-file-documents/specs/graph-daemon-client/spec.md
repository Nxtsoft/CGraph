## ADDED Requirements

### Requirement: A file's body document covers its unenclosed text and path
The body index SHALL give each indexed file node a document made of the terms on lines no other
indexed node's span in that file covers, plus the terms of the file node's label (its
repo-relative path). Symbol documents SHALL be unchanged, so no line's terms count in both a
symbol's document and its file's.

#### Scenario: An anonymous top-level test block is searchable
- **GIVEN** a test file whose cases are anonymous `test('...', () => ...)` callbacks
- **WHEN** a prose query uses words from one of those cases
- **THEN** the file node ranks by its body document

#### Scenario: A symbol's body is not counted twice
- **WHEN** a query's terms appear only inside a named function's span
- **THEN** that function's document matches and its file's document does not

### Requirement: Context reaches a seed's tests at depth 1
When `max_depth` is at least 1, `context` SHALL admit every file node joined by a `tests` edge to
a seed's file node (a source seed's tests, or a test seed's subject) at depth 1 with
`via = "tests"`, keeping a shallower depth already reached. Admitted counterparts SHALL NOT be
expanded further. With `max_depth` 0 nothing SHALL be admitted.

#### Scenario: A symbol's test file outranks its file's imports
- **GIVEN** `cleanPath` in `path.go`, which imports two high-centrality files, and
  `path_test.go -tests-> path.go`
- **WHEN** `context` is requested for `cleanPath`
- **THEN** `path_test.go` is included at depth 1, ahead of both imported files
