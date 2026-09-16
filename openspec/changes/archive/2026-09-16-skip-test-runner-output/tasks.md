# Tasks

- [x] 1.1 `path_ignore_test.cpp`: `playwright-report`, `test-results`, `blob-report` skip; `tests`,
      `test-utils`, `playwright` do not.
- [x] 1.2 `is_skipped_directory` lists the three names with the measurement that motivated them.
- [x] 1.3 Spec delta: test-runner output named among the excluded tree kinds.
- [x] 2.1 `ctest --preset default`: 78/78 pass (this Mac, Debug preset).
- [x] 2.2 Before/after on `frontend`: nodes 17,439 -> 14,302 (3,137 under the Playwright dirs -> 0);
      design entry points 1,349 -> 1,071 with zero under `playwright-report/`; clone classes 216 -> 170
      with the 46 bundle-only classes gone.
- [ ] 2.3 Obtain non-author review and merge.
