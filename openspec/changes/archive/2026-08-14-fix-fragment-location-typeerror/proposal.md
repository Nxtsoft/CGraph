## Why

A single semantic enrichment fragment with a type-confused `source_location` field crashes the
resident daemon. `parse_source_location` reads each component with nlohmann's
`json::value(key, 0U)`, which **throws** `type_error.302` on a type mismatch, and no caller on
the drop-ingest path catches it. Reproduced live on this machine:

```
# a document fragment with "source_location": {"start_line": "9"}  (string, not int)
$ graphd --root <proj>        # daemon up, serving
# atomically drop chunk_00.json into cgraph-out/semantic-drop/
graphd: libc++abi: terminating due to uncaught exception of type
  nlohmann::type_error: [json.exception.type_error.302] type must be number, but is string
# daemon DEAD (SIGABRT)
```

This defeats the contract's central guarantee -- `docs/host-skill-contract.md`: "Malformed JSON
or schema violations are rejected and must not alter the graph snapshot." A `document`/`media`
node carrying a `source_location` is exactly the shape enrichment hosts emit, so an LLM writing
a string where an int is expected is a one-line remote kill of the per-project daemon.

Every other optional field in `fragment_json.cpp` tolerates a type-confused value by degrading
to a default (`optional_string`, `parse_confidence_fields`, `parse_properties` all guard with
`is_*` before reading). Only `source_location` uses the throwing `value()` accessor. Independent
drift-check confirmed the escape path (no `try/catch` in `parse_fragment`,
`validate_semantic_fragment_*`, `ingest_semantic_fragment`, or the `daemon_server.cpp` drop-ingest
call sites), and confirmed a companion bug: numeric out-of-range fields wrap silently
(`-5` -> `4294967291`, `2^32` -> `0`).

## What Changes

- `parse_source_location` reads each component only when it is a non-negative integer JSON
  number, degrading absent/null/string/bool/float/negative/oversized values to 0 -- the same
  "tolerate garbage, reject only on structure" rule the rest of the parser follows. Parsing a
  fragment becomes total: no fragment shape can throw out of `parse_fragment`.
- A `source_location` object with no readable numeric component (every field absent, or all
  string/null/bool/float/negative/oversized) becomes absent rather than a fabricated
  line-0/column-0 site; a location with at least one readable field is present and reflects
  only the readable fields.
- Regression test in `fragment_json_test.cpp`: a fragment whose location fields are
  string/null/bool/int must parse without throwing and must not fabricate an all-zero site.
- Non-goals: no change to which fragments are structurally rejected; no new validation errors;
  no change to the fragment schema hosts write.

## Impact

- **Fixes a remote daemon-kill.** One malformed enrichment drop no longer aborts `graphd`;
  it ingests with a degraded/absent location, keeping the daemon and graph intact.
- **Touches:** `src/engine/fragment_json.cpp`, `tests/smoke/fragment_json_test.cpp`.
- Also closes the numeric-wrap drift (negative/oversized location components).

## Capabilities

### Modified Capabilities

- `semantic-fragment-ingest` -- fragment parsing must never throw on a type-confused field.
