# Fix: an `imports` edge naming the exact callee must break an ambiguity tie (issue #70)

## Why

When a callee name is declared in more than one file, `resolve_raw_calls` drops the call as
`dropped_ambiguous` — even when the caller's file has an `imports` edge naming **the exact
declaration it meant**. The disambiguating fact is already in the graph and already loaded
into `imported_symbols`, but it is only consulted *after* the uniqueness test has decided
whether an edge exists.

Reduced to four Python files: `storage.py` and `cache.py` both declare `write_text`;
`report.py` says `from storage import write_text` and calls it. The graph contains

```
src/report.py --imports--> write_text[storage.py]   EXTRACTED
```

and the call is still dropped. Delete `cache.py`'s copy so the name is unique and the same
call resolves — graded `EXTRACTED` *because of that same import*. So the import is read, and
it is load-bearing for confidence; it just runs too late to decide existence.

The exactly-one-provable-candidate rule is right and is not being relaxed. The defect is that
an explicit import **is** proof and was not counted as such.

## What Changes

- `graph_builder.cpp::resolve_raw_calls`, tier 2: on the ambiguous path, before dropping,
  narrow the candidate set to those the caller's file imports **by name** (the target of an
  `imports` or `re_exports` edge) and re-apply the ordinary rule to the survivors:
  - exactly one survivor → resolves, graded `EXTRACTED` (the import is the proof);
  - several survivors sharing one file → an imported overload set, resolved by the existing
    `resolve_overload_set` under issue #52's rule (edge to every member, `INFERRED`);
  - anything else → still `dropped_ambiguous`.
- A module-level `imports_from` edge deliberately does **not** narrow the set. It names a
  file, not a declaration, and the bare-name call it would rescue is spelled `pkg.fn()` — a
  member call this tier never sees.
- New smoke test `import_disambiguation_test.cpp`: six cases — the tie broken by an import,
  the tie *not* broken without one, two imports of one name picking neither, an imported
  overload set edging to every member, `re_exports` carrying the same proof, and a module
  import failing to break a tie.

### Non-goals
- The member-call tiers (2a/2b). Most `dropped_ambiguous` in practice is `obj.method()`,
  which never reaches this tier — see the honest impact numbers below.
- Transitive re-export chains. A direct `imports`/`re_exports` edge is required; following
  A → B → C is a separate change.
- Any change to confidence grading for calls that already resolved.

## Impact

Measured against `f6e0c06` (the pre-fix binary) on the same machine, same repos:

| Repo | Language | `dropped_ambiguous` | Edges |
|---|---|---|---|
| pallets/itsdangerous | Python | 2 → **0** | 391 → 393 |
| tokio-rs/tokio | Rust | 7,162 → **6,797** (−365) | 30,351 → 30,609 |
| clap-rs/clap | Rust | 4,274 → **4,258** (−16) | 19,056 → 19,069 |
| BurntSushi/ripgrep | Rust | 731 → **729** (−2) | 17,901 → 17,903 |
| stleary/JSON-java | Java | 10 → 10 | unchanged |
| google/gson | Java | 368 → 368 | unchanged |
| gorilla/mux | Go | 16 → 16 | unchanged |
| CGraph `src/` | C++ | 0 → 0 | unchanged |

**The payoff is real but much smaller than the raw counts suggest, and that is worth stating
plainly.** Issue #70 asked what fraction of ambiguous drops have an import behind them; the
answer is 5.1% on tokio, 0.4% on clap, and 0% on the Java and Go repos. The reason is that
this tier only sees non-member calls: the bulk of `dropped_ambiguous` everywhere is
`self.foo()` / `obj.method()`, where the receiver type is unknown and no import names a
declaration. The change is strictly additive — no repo lost an edge, and every partition
still balances.

Determinism and cost are unaffected: two runs over tokio produce a byte-identical
`graph.json`, and the resolve phase across three paired runs measures 169–229 ms before and
182–217 ms after, inside run-to-run noise on a ~9.5 s total build.

- **Touches:** `src/engine/graph_builder.cpp`, `tests/smoke/import_disambiguation_test.cpp`,
  `tests/smoke/CMakeLists.txt`.

## Capabilities

### Modified Capabilities
- `deterministic-graph-pipeline` — project-wide call resolution admits symbol-import evidence
  as a tiebreaker.
