# Tasks

## 1. Narrow ambiguous candidates by symbol-import evidence
- [x] 1.1 Add a `resolve_imported_candidate` lambda in `resolve_raw_calls` that filters a
      candidate set to the targets of the caller file's `imports`/`re_exports` edges.
- [x] 1.2 One survivor resolves and is graded `EXTRACTED`; several survivors in one file go
      through the existing `resolve_overload_set`; anything else returns false.
- [x] 1.3 Call it on tier 2's ambiguous path, immediately before `++tally.dropped_ambiguous`.
- [x] 1.4 Leave `imported_modules` confidence-only and record why in the comment.

## 2. Test
- [x] 2.1 New `tests/smoke/import_disambiguation_test.cpp` with six cases: tie broken by an
      import; tie preserved without one; two imports of one name picking neither; imported
      overload set edging to every member; `re_exports` as proof; module import not enough.
- [x] 2.2 Register the target in `tests/smoke/CMakeLists.txt`.
- [x] 2.3 Verify the test FAILS with the new branch disabled and passes with it enabled.
- [x] 2.4 Full suite green — 76/76 passed, 0 failed.

## 3. Measure
- [x] 3.1 Before/after `dropped_ambiguous` and edge counts on eight repos across five
      languages; confirm every partition still balances and no repo loses an edge.
- [x] 3.2 Answer issue #70's open question with a number: the share of ambiguous drops that
      carry import evidence (5.1% tokio, 0.4% clap, 0% Java/Go).
- [x] 3.3 Confirm determinism (byte-identical `graph.json` over two tokio runs) and that the
      resolve phase stays within run-to-run noise.

## 4. Spec
- [x] 4.1 Delta for `deterministic-graph-pipeline`: extend the "Call sites are keyed on the
      callee name" requirement with the import-narrowing rule and five scenarios.
