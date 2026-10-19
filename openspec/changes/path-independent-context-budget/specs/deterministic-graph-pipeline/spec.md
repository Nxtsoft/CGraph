## MODIFIED Requirements

### Requirement: Retrieval budget is not spent on the project's location
No node identifier in a context entry SHALL include any portion of a path above the project
root, so that the identifier part of every entry's cost is the same however deeply the project
is checked out. A context entry's `source_file` SHALL name a file under the project root by its
path relative to that root, and the `context` response SHALL state the root once, as a
top-level `root`, outside the entries the budget is charged for. A file outside the root keeps
its path as recorded. The budget SHALL be charged for the entries as emitted, so the same tree
built at two roots of different length SHALL pack the same entries, in the same order, at the
same charge. `change-context` SHALL pack its context the same way, with each file relative to
the root of the snapshot it came from.

#### Scenario: Identifier cost does not move with checkout depth
- **WHEN** the same tree is built from two roots of different length
- **THEN** every node identifier, and therefore every identifier's serialized cost, is
  byte-identical between the two builds

#### Scenario: Context packing does not move with checkout depth
- **WHEN** the same tree is built from two roots of different length and `context` is asked
  the same question at a budget that sheds candidates, under greedy and under knapsack packing
- **THEN** both responses carry identical `focus` and `included` entries in identical order,
  the same `omitted` count and the same `tokens_used`
- **AND** each entry's `source_file` is relative to the project root, which the response
  states once as `root`
