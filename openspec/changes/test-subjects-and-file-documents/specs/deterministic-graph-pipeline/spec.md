## ADDED Requirements

### Requirement: Test files link to the source file they test
After call, relation, contract and interface-dispatch resolution, the pipeline (one-shot and
incremental alike) SHALL add an inferred `tests` edge from each test file node to the source file
node its name pairs with. A test file SHALL be one `is_test_source_path` classifies by its
repo-relative path, or whose stem is `test`/`tests` or carries a test marker (`test_x`, `x_test`,
`x_tests`, `x.test`, `x.spec`, `x_spec`). The pair SHALL be the non-test file whose stem equals the
test's stem with one marker removed. A pair in the test's own directory SHALL always link. Beyond
it, only a test under a test-root directory (`test`, `tests`, `__tests__`, `spec`, `specs`, `e2e`,
`integrationTest`) MAY link, and only to the unique matching file inside its package (the path
before the first test root), else to the one matching file in the directory its test-root-free
path mirrors. Generic stems (`main`, `mod`, `lib`, `index`, `__init__`, `utils`, `util`,
`helpers`, `common`, `conftest`, `setup`) SHALL pair only in-directory, and files under
`examples`, `example`, `docs`, `doc`, `benches`, `bench` or `benchmarks` SHALL never be subjects.
Anything else SHALL produce no edge. Re-running the pass SHALL add nothing.

#### Scenario: A Go test links to the file beside it
- **WHEN** `path_test.go` and `path.go` share a directory
- **THEN** the graph has `path_test.go -tests-> path.go`

#### Scenario: A same-named file in another package is not a guess
- **GIVEN** `modules/reverseproxy/headers_test.go` and a lone `modules/headers/headers.go`
- **THEN** no `tests` edge joins them

#### Scenario: A test under a test root mirrors its subject's directory
- **GIVEN** `tests/http/test_client.py`, `http/client.py` and `db/client.py`
- **THEN** the test links to `http/client.py` only
