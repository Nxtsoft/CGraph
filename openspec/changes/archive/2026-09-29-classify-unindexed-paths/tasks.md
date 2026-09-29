## 1. Classification

- [x] 1.1 Add a failing test covering all four situations: a gitignored directory, a dependency
      directory, a visited-but-unextracted build file, and an extracted source file.
- [x] 1.2 Implement `classify_project_paths`, mirroring `detect_project_files`'s predicates and
      order exactly, so the verdict cannot drift from what detection actually did.
- [x] 1.3 Record ignored directories by subtree root rather than enumerating their contents.
- [x] 1.4 Treat a detected file that produced no graph node as `unindexed`, not `indexed`.

## 2. Export

- [x] 2.1 Write `paths.json` from `write_exports`.
- [x] 2.2 Confirm `graph.json` is byte-identical before and after this change.

## 3. Verification

- [x] 3.1 Run the new test and the full default suite.
- [x] 3.2 Run the real flow on this repository and confirm `research/` is reported as an ignored
      directory while `CMakeLists.txt` is reported as unindexed.
- [x] 3.3 Run OpenSpec validation.
