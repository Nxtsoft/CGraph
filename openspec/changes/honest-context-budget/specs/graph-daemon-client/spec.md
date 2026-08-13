## ADDED Requirements

### Requirement: Context bundles respect their token budget
`tokens_used` SHALL report the true serialized cost of the returned `included` array, measured identically for every packing mode, and the serialized size of `included` SHALL NOT exceed the requested `budget`.

The knapsack's internal ranking weight remains the node's capped source-slice cost. That is a selection heuristic, not a report: weighing the full serialized entry flattens the weight spread and degenerates the knapsack toward greedy, so the ranking weight SHALL NOT change and SHALL NOT be reported as `tokens_used`. Entries dropped by the post-selection true-cost pass SHALL be counted in the existing `omitted` field.

The focal entry SHALL be charged first and SHALL NOT be dropped, so a budget too small to fit it still identifies the symbol the caller asked about.

An entry without a snippet SHALL be marked in every packing mode: `snippet_omitted: true` when the node has a source extent (a failed read, or a deliberate brief-only row that still carries file and line), and `snippet_unavailable: true` when the node's kind has no source extent at all (documents, concepts, media) and the entry is a followable pointer by design.

The response's `budget_basis` field SHALL report `measured_serialized_tokens`, replacing the pre-ceiling values (`estimated_source_slice_tokens`, `projected_entry_tokens`); consumers switching on the old values are consumers of the overshoot being removed.

A mistyped request parameter SHALL yield an error response frame; it SHALL NOT terminate the daemon. A non-positive `budget` SHALL behave as `budget: 0` (focal-only, truncated).

#### Scenario: The default gather mode respects its budget
- **GIVEN** a resident daemon with a built graph
- **WHEN** `context` is called with `budget: 3000` and no `gather` parameter
- **THEN** the serialized `included` array is at most 3000 estimated tokens
- **AND** the response is small enough for an MCP client to accept

#### Scenario: A small budget still returns the focal symbol
- **WHEN** `context` is called with a budget too small to fit the focal entry
- **THEN** the focal entry is still present
- **AND** `included` is empty rather than over budget

#### Scenario: A snippet-less entry is marked in every packing mode
- **WHEN** an included entry's source snippet cannot be read
- **THEN** the entry carries `snippet_omitted: true` under both knapsack and greedy packing
- **AND** an entry for a structurally sourceless kind carries `snippet_unavailable: true` instead

#### Scenario: A hostile parameter cannot kill the daemon
- **WHEN** `context` is called with a mistyped parameter such as `packing: 7` or `budget: -1`
- **THEN** the daemon returns an error frame (or a floored, truncated response for the negative budget)
- **AND** the daemon keeps serving subsequent requests

### Requirement: The default packing mode is justified at equal cost
The default packing mode SHALL be the one that measures best when every mode is held to the same enforced budget. A packing mode SHALL NOT be selected as default on the strength of a comparison in which it returned more than its stated budget.

#### Scenario: Packing modes are compared at equal enforced budgets
- **WHEN** packing modes are compared for the purpose of choosing a default
- **THEN** each is measured with its serialized `included` array within the same budget
- **AND** the recorded comparison states the budget enforced for each
