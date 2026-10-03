## 1. Tests

- [x] 1.1 `cli_main_test`: under a file-size limit `seam fuse`, `seam discover` and the one-shot build exit non-zero, name the file, leave no truncated file, no temp file and no seam marker, and keep a prior `graph.json` (fails on origin/main: all three exit 0 over truncated files).
- [x] 1.2 `daemon_lifecycle_test`: a persist that runs out of space part-way returns false and keeps the last-known-good `graph.json` (fails on origin/main: the truncated temp was renamed over it).
- [x] 1.3 `atomic_write_test`: exact bytes, overwrite, short write reported with the prior file kept and no temp, missing directory throws `FileWriteError` naming the path.

## 2. Implementation

- [x] 2.1 `write_file_atomically` / `try_write_file_atomically`; the three private temp-and-rename copies call it.
- [x] 2.2 Every CLI and engine output write goes through it; `cgraph` exits 1 on `FileWriteError`; the daemon logs.

## 3. Gates

- [x] 3.1 Full suite (`ctest --test-dir build/default -j6`).
- [x] 3.2 Probe `seam discover` / `seam fuse` for both systems byte-identical to bin-v0.8.1.
- [x] 3.3 A real full tmpfs (`unshare -r -m`, `mount -t tmpfs -o size=4k`): origin/main `seam fuse` exits 0 over a 4096-byte `graph.json`; this change exits 1 with `No space left on device` and leaves the directory empty.
