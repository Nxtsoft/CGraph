## ADDED Requirements

### Requirement: Context bundles respect their token budget
`tokens_used` SHALL report the true serialized cost of the returned `included` array, measured identically for every packing mode, and the serialized size of `included` SHALL NOT exceed the requested `budget`.

The knapsack's internal ranking weight remains the node's capped source-slice cost. That is a selection heuristic, not a report: weighing the full serialized entry flattens the weight spread and degenerates the knapsack toward greedy, so the ranking weight SHALL NOT change and SHALL NOT be reported as `tokens_used`. Entries dropped by the post-selection true-cost pass SHALL be counted in the existing `omitted` field. An entry whose source snippet could not be read SHALL carry `snippet_omitted: true` in every packing mode, so a caller can distinguish a deliberate summary row from a failed read.

#### Scenario: The default gather mode respects its budget
- **GIVEN** a resident daemon with a built graph
- **WHEN** `context` is called with `budget: 3000` and no `gather` parameter
- **THEN** the serialized `included` array is at most 3000 estimated tokens
- **AND** `tokens_used` is within tolerance of that measured size
- **AND** the response is small enough for an MCP client to accept

#### Scenario: A small budget still returns the focal symbol
- **WHEN** `context` is called with `budget: 1000`
- **THEN** the focal entry is present and the budget is still respected

#### Scenario: A snippet-less entry is marked in every packing mode
- **WHEN** an included entry's source snippet cannot be read
- **THEN** the entry carries `snippet_omitted: true` under both knapsack and greedy packing
