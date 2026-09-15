# Tasks

## 1. Types view
- [x] 1.1 `report_test.cpp::test_types_view`: two `FileState` declarations -> one duplicate row with
      overlap 0.40; `Point`/`Vec3` identical; `Base` ⊂ `Extended` reported as subset at 0.75; a 0.67
      pair and a pair under the member floor not reported; `unused` lists types whose only incoming
      edges are structural or from their own fields and not referenced, constructed or inherited
      types; threshold/min_members/scope/include_tests widen or narrow the report.
- [x] 1.2 `report_test.cpp::test_types_budget_and_renderers`: one token short sheds unused first; a
      tight budget keeps duplicates only; budget 0 sheds nothing; json and markdown shapes.
- [x] 1.3 `report_test.cpp::test_types_envelope`: json and markdown envelopes, mermaid/svg refused
      with `report_format_unsupported`, threshold and min_members validated; `types` leaves the
      reserved list in `test_daemon_envelope`.
- [x] 1.4 `TypeRef`/`DuplicateTypes`/`TypeOverlap`/`TypesReport`, `build_types_report`,
      `shed_to_budget`, `types_report_json`, `render_types_markdown`, `render_types_report`,
      `report_response` dispatch; `ReportRequest.min_members`.

## 2. Surfaces and docs
- [x] 2.1 CLI: `cgraph report types`, `--threshold`, `--min-members`, markdown default, generic totals.
- [x] 2.2 MCP: `graph_report` description, `threshold`, `min_members`.
- [x] 2.3 `docs/host-skill-contract.md`, `integrations/skills/cgraph/SKILL.md`, `README.md`,
      `README.zh-CN.md`.

## 3. Verification
- [x] 3.1 `ctest --preset default`: 77/77 pass (this Mac, Debug preset).
- [x] 3.2 Measured on CGraph's own tree from the built daemon (190 files): 146 types, 120 with
      members; duplicates `PendingEvent` (1.00) then `FileState` (0.40); 0 identical, 0 overlaps; 56
      unreferenced, of which grep finds 25 named elsewhere (C++ references resolve through direct
      includes only -- recorded as a limitation in the proposal); `FileCacheEntry` not flagged.
- [x] 3.3 Measured on `frontend` (1,261 TypeScript files, 17,439 nodes) in 0.41 s: 45 identical
      groups (widest 17 members), 94 duplicates, 464 overlaps (457 subset, 7 overlap), 1,180
      unreferenced; 203k tokens unbudgeted, 6,000 at the default budget keeping all identical groups
      and 40 duplicates.
- [ ] 3.4 Obtain non-author review and merge.
