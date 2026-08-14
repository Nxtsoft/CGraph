# Tasks

## 1. Reproduce

- [x] 1.1 Prove the crash live: a fragment with `"source_location": {"start_line": "9"}`
      dropped into a running daemon's `semantic-drop` dir aborts `graphd` (SIGABRT,
      uncaught `type_error.302`). Confirmed independently by drift-check against the code path.

## 2. Fix

- [x] 2.1 Add a failing test in `tests/smoke/fragment_json_test.cpp`: a fragment with
      string/null/bool/int location fields must parse without throwing and must not fabricate
      an all-zero site. (Fails with SIGABRT before the fix.)
- [x] 2.2 In `src/engine/fragment_json.cpp`, read each `source_location` component only when it
      is a non-negative integer JSON number; degrade every other value to 0; return absent when
      no numeric component is present. Parsing must be total.

## 3. Prove it

- [x] 3.1 Regression test green; the pre-fix binary aborts on it.
- [x] 3.2 Live daemon survives the exact poison drop that killed it, stays responsive to
      queries, and shuts down cleanly.
- [x] 3.3 `ctest --preset default` green (66/66 local). Sanitizers leg via CI (ASan hangs at
      startup machine-wide on this Darwin box, unrelated to the change).
