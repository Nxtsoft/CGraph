## ADDED Requirements

### Requirement: The precomputed layout uses one algorithm at every graph size

`write_layout` SHALL compute node coordinates with a single force-directed algorithm (Fruchterman-Reingold with igraph's grid approximation) regardless of the node count, and SHALL NOT select a different layout algorithm above any size threshold. The result SHALL be rescaled to the canvas-sized square described by the interactive-view requirement, so a graph of any size receives finite coordinates spanning that side.

#### Scenario: A graph past two thousand nodes lays out like a small one
- **GIVEN** a connected graph of 2,500 nodes
- **WHEN** `detect_communities` runs
- **THEN** every node carries finite `x`/`y` properties, the wider axis spans `max(720, 30 * sqrt(2500))` pixels, and the pass completes in the same order of time as a 2,000-node graph
