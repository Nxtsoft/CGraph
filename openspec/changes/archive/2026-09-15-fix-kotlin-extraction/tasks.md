# Tasks

- [x] Diagnose why Kotlin extracts an empty graph (no named fields on the fwcd grammar's
  declarations / call_expression).
- [x] Add `kotlin_symbol_name` positional name resolver (type_identifier / simple_identifier).
- [x] Add `kotlin_callee_name` positional callee resolver (navigation_expression / simple_identifier).
- [x] Extend `add_raw_call` to consult `resolve_callee_name` when a call node has no accessor field.
- [x] Rewrite `kotlin_config` to use the resolvers; drop the phantom `interface_declaration` and the
  ineffective field lists.
- [x] Add `check_kotlin_extraction` covering class/object/interface nodes, function names, a plain
  call, and a reduced navigation call.
- [x] Verify: `configured_extractors_test` and `extractor_goldens_test` pass; a real Kotlin repo goes
  from 0 edges to full symbol + call extraction.
