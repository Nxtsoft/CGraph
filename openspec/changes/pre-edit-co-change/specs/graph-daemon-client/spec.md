## ADDED Requirements

### Requirement: The daemon names the files likely to change with an edit
The daemon SHALL serve a `co-change` op taking `id` (a file node id, required), `lines` (1-based inclusive `[first, last]` ranges; absent or empty for the whole file) and `limit` (default 5), and SHALL answer `found`, `edited` (the labels of the innermost symbols of the file overlapping a range; every symbol for the whole file), `candidates` and `files`, each with `source_file`, a one-line `why` and its `signal`. It SHALL rank three lists and interleave them, taking the next unlisted file from each in turn: `callers` (files whose code reaches an edited symbol directly, then through one intermediate symbol; shallower first, then more edges, a call named before an import), `tests` (the file's test or source counterpart by name, same directory first) and `linked` (other files joined to the file or its symbols by an edge in either direction, more edges first). It SHALL read the published snapshot only, in time linear in its nodes and edges, and SHALL answer `found: false` with no files for an id that is no file node.

#### Scenario: An edit to a called function lists its caller, its test and the rest
- **GIVEN** `metrics.py` defining `record` and `other`, `routes.py` importing and calling `record`, `service.py` calling `routes.handle`, `unrelated.py` calling `other`, and `test_metrics.py`
- **WHEN** `co-change` runs for `metrics.py` with lines `[[1, 2]]`
- **THEN** `edited` is `["record"]` and `files` is `routes.py` (`calls `record``), `test_metrics.py` (`test for this file`), `unrelated.py`, `service.py` (`calls `handle`, which depends on the edited code`)

#### Scenario: A method edit names the method
- **WHEN** the lines fall inside a method of a class
- **THEN** `edited` names the method, not the class

### Requirement: The daemon skips requests whose client has gone
After reading a request frame, the daemon SHALL check whether the client has already hung up and, if it has, SHALL NOT compute or answer the request and SHALL count it in `status` `ops.abandoned`. A client that is still connected SHALL be answered as before.

#### Scenario: An abandoned query is not computed
- **GIVEN** a daemon busy with another connection
- **WHEN** a client sends a whole `query` frame and closes before the daemon reads it
- **THEN** `ops.abandoned` grows by one, the query count does not change, and a later live query is answered
