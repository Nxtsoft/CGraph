## ADDED Requirements

### Requirement: Node lookup resolves exact keys only and fails loud

Every op that takes a node key (`explain` and `impact` via `id`; `path` via `source` and `target`; `context` via `id`) SHALL resolve it by exact id first. When the key is not an id, the engine SHALL try the exact label, then the label's leading symbol token compared case-insensitively; each of those tiers SHALL resolve only when exactly one node matches. When a tier matches several nodes, the key SHALL be reported as ambiguous: the response carries the op's not-found shape (`found: false`, `focus: null`, or `<endpoint>_found: false`) plus `ambiguous: true`, `candidate_count`, and the matching nodes in `suggestions`, ordered most central first, then by label, then by id, capped at the suggestion limit. An empty key SHALL match nothing. A `context` request whose `id` is ambiguous SHALL NOT fall through to its free-text `query`.

A request for `explain` or `impact` with no `id`, for `path` with no `source` or no `target`, or for `context` with none of `id`, `q`, `query` -- absent, null, or empty string -- SHALL be refused before dispatch with the ordinary `{ok: false, error}` envelope naming the missing parameter.

#### Scenario: An empty id is refused, not searched
- **GIVEN** a graph containing a node labelled `(anonymous)`
- **WHEN** `explain` is called with `{}` or `{"id": ""}`
- **THEN** the response is `ok: false` with an error naming `id`, and no node is returned

#### Scenario: A shared label is ambiguous
- **GIVEN** two function nodes labelled `write_file` in different files
- **WHEN** `explain` is called with `{"id": "write_file"}`
- **THEN** the response is `found: false`, `ambiguous: true`, `candidate_count: 2`, and
  `suggestions` lists both nodes, the more central first

#### Scenario: A shared bare name is ambiguous
- **GIVEN** nodes labelled `Unique(int)` and `unique`
- **WHEN** `impact` is called with `{"id": "UNIQUE"}`
- **THEN** the response is `found: false` and `ambiguous: true` with both as candidates

#### Scenario: The canonical id always resolves
- **GIVEN** the two `write_file` nodes above
- **WHEN** `explain` is called with one of their ids
- **THEN** that node is returned with `found: true`

#### Scenario: A unique exact label or bare name still resolves
- **WHEN** `explain` is called with a label exactly one node carries, or a bare symbol name
  exactly one node's label starts with
- **THEN** that node is returned and the response echoes its canonical id

#### Scenario: An ambiguous path endpoint is flagged
- **WHEN** `path` is called with an ambiguous `source` and a resolvable `target`
- **THEN** the response carries `source_found: false`, `source_ambiguous: true`, and the
  candidates in `source_suggestions`, and no `target_found`
