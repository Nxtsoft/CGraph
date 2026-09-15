## ADDED Requirements

### Requirement: One-shot builds write the module diagram
`write_exports` SHALL take the project root and write `modules.mmd` (Mermaid) and `modules.svg` next to `graph.json`, built from the whole project with no budget and test roots excluded, so a human can open the architecture map without a running daemon.

#### Scenario: Exports include the module diagram
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/modules.mmd` and `DIR/modules.svg` exist alongside `graph.json`

## MODIFIED Requirements

### Requirement: Interactive HTML view reveals community structure
The interactive `graph.html` export SHALL position nodes so that computed community assignments are visible as spatially distinct regions, rather than using community only for color while leaving the layout a uniform frame-filling cloud. The layout SHALL remain deterministic for a given graph (no use of `Math.random`).

The engine-side layout (`write_layout`) SHALL emit coordinates rescaled to a canvas-sized square that grows with the square root of the node count, so the viewer's first paint spreads the graph across the canvas instead of collapsing igraph's unit-scale output into one blob. Above 500 nodes the viewer SHALL open community-collapsed: each community drawn once as a sized super-node with aggregated edges, expanded by clicking it, by a search match, or by the expand-all control.

#### Scenario: Communities render as separated regions
- **WHEN** the pipeline exports `graph.html` for a graph with multiple detected communities and the view settles
- **THEN** nodes of the same community are drawn closer to one another than to nodes of other communities, so distinct communities read as separate regions

#### Scenario: Layout is not clamped to a viewport box
- **WHEN** the layout settles for a graph larger than the viewport
- **THEN** nodes relax in open space without piling against fixed canvas edges, and the view auto-fits the whole graph so it is visible on load

#### Scenario: Layout is deterministic
- **WHEN** `graph.html` is generated twice for the same graph
- **THEN** the generated layout logic uses only seeded placement (no `Math.random`), so the same graph produces the same layout each load

#### Scenario: Large graphs first paint readable
- **WHEN** `graph.html` opens on a graph of more than 500 nodes
- **THEN** the first paint shows one node per community with edges between communities, labelled and readable without zooming, and clicking a community reveals its members at their precomputed positions
