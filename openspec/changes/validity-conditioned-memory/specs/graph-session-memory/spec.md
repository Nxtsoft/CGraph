## ADDED Requirements

### Requirement: Touches are anchored to their source span at write time
For each `touches` entry that resolves, the `remember` op SHALL store on the `concerns` edge an
`anchor_sha256` property. Its value is the sha256 of the resolved node's own source span (the lines
from its `source_location` start to end, read in full from its `source_file`), not a hash of the
whole file. The span is cut at the snapshot's line numbers, so an anchor SHALL be stored only
when the file's current content hash equals the snapshot's `source_hashes` entry for it. When the
node has no location, its span cannot be read, or its file was edited after the snapshot extracted
it, the edge SHALL carry no anchor. The response SHALL report `anchored`, the number of edges that carry an anchor. The anchor
SHALL travel in the checkpoint's sidecar fragment like any other edge property.

#### Scenario: A file edited after the snapshot is not anchored
- **WHEN** a touched function's file is edited (shifting its lines) and `remember` runs before the
  snapshot re-extracts it
- **THEN** the edge carries no `anchor_sha256` and recall reports the link `unanchored`, never an
  anchor cut from the wrong lines

#### Scenario: Located symbols are anchored and a location-less node is not
- **WHEN** `remember` touches two functions that have source locations and one module node that
  has none
- **THEN** the response reports `concerns` 3 and `anchored` 2, and only the two function edges
  carry `anchor_sha256`

### Requirement: Recall grades each touched symbol against its anchor
For each `concerns` link it returns, `recall` SHALL include a `validity` of `valid` when the target's
current span hashes to the edge's `anchor_sha256`, `changed` when it does not, can no longer be
read, or its file was edited after the snapshot extracted it, and `unanchored` when the edge carries
no anchor. Each checkpoint SHALL include `gone`, the
touch keys whose symbols no longer exist, and a `validity` of `stale` when any link is `changed` or
`gone` is non-empty, `unverified` when neither holds but some link is `unanchored` or there are no
links, and `valid` otherwise. Validity SHALL NOT filter, reorder, or limit the checkpoints recall
returns.

#### Scenario: An edit elsewhere in the same file does not change a link
- **WHEN** a checkpoint touches two functions in one file and only the second function's body is
  edited
- **THEN** recall reports the first link `valid`, the second `changed`, and the checkpoint `stale`

#### Scenario: A link into a file not yet re-extracted reads changed
- **WHEN** a touched file is edited and recall runs before the snapshot re-extracts it
- **THEN** every link into that file reads `changed` until the snapshot catches up

#### Scenario: Code that only moved stays valid
- **WHEN** a touched function's text is unchanged but it now starts further down the file
- **THEN** recall reports that link `valid`

#### Scenario: A checkpoint written before anchoring is unverified
- **WHEN** a checkpoint's `concerns` edges carry no `anchor_sha256`
- **THEN** recall reports each of those links `unanchored` and, with nothing changed or gone, the
  checkpoint `unverified`

### Requirement: The memory overlay records touches it cannot re-bind
When `overlay_memory_fragments` drops a `concerns` edge because neither its target id nor its touch
key resolves, it SHALL record that touch key on the checkpoint node's `gone_touches` property. On
every overlay it SHALL recompute `gone_touches` for each overlaid checkpoint from that overlay's
dropped edges, and SHALL remove the property when none were dropped, so that repeated overlays
converge and a restored symbol clears it.

#### Scenario: A deleted symbol is reported as gone
- **WHEN** a touched function is deleted, the graph is rebuilt, and the memory sidecars are
  re-overlaid
- **THEN** recall lists the function's touch key under `gone` and reports the checkpoint `stale`

#### Scenario: Repeated overlays converge
- **WHEN** the same sidecars are overlaid a second time onto the same graph
- **THEN** node and edge counts and every checkpoint's `gone` are unchanged

#### Scenario: A symbol restored in the same graph clears gone
- **WHEN** the deleted function reappears in the live graph and the sidecars are overlaid again
  without a full republish
- **THEN** the overlay removes `gone_touches` and recall's `gone` is empty

#### Scenario: A restored symbol clears gone
- **WHEN** the deleted function returns with its original text and the sidecars are re-overlaid
- **THEN** `gone` is empty and recall reports the link `valid` against its original anchor

## MODIFIED Requirements

### Requirement: Recall tolerates dangling concerns targets
Recall SHALL NOT return a link for any `concerns` target that no longer resolves to a node (e.g. a
code node renamed or removed by a rebuild after the checkpoint was written), and SHALL NOT error.
It SHALL return the checkpoint with only its resolvable links and SHALL list the unresolvable
target's touch key (or its target id when the edge has no touch key) under the checkpoint's `gone`.

#### Scenario: A removed concerns target is skipped on recall
- **WHEN** a checkpoint concerns a code node that is later removed and recall runs
- **THEN** the checkpoint is returned without the missing link and without error, and the missing
  touch is listed under `gone`
