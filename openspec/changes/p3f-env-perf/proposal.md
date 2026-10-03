# Env reads: per-file binding tables and two missing shadows

## Why

The #161 re-review left two items open in `src/engine/env_contracts.cpp`:

- **Quadratic lookups.** Resolving the object of every upper-snake-case member (`Enum.MEMBER`, `Number.MAX_SAFE_INTEGER`, `env.X`) walked out through each enclosing scope, and each function on the way (and, at module level, the program) was searched for a hoisted `var` by walking its entire body. A file with N such reads and M statements cost O(N x M).
- **Two missing shadows.** A `let` / `const` directly in a `switch` case (no braces: `switch_case` holds statements directly, not in a `statement_block`) and a local TypeScript `enum env` did not shadow a module-level typed env `const env`, so `env.X` inside them was read as an env variable.

## What Changes

- What each block-like scope binds (first binding of each name among its statements) and what each function body or the program hoists through `var` is computed once per scope per file, under an `EnvContractsFileScope` held by `extract_javascript` / `extract_typescript` / `extract_tsx` (the `HeaderContractsFileScope` pattern). Without a scope each lookup computes the tables for the scopes it passes, with the same result. One traversal (`any_bound`) serves both the name test and the table build, so the two cannot drift.
- A `switch_body` is a scope: the statements of all its cases bind for the whole switch (JavaScript's rule).
- `enum_declaration` binds its name like a class.

## Impact

- `src/engine/env_contracts.cpp`, `src/engine/include/cgraph/env_contracts.hpp`, `tests/smoke/env_contracts_test.cpp`; a one-line scope in each of the three JS/TS entry points in `javascript_extractor.cpp`; `index_persistence.cpp` `logic-21` (the shadowing fix changes output for files with the switch-case / local-enum pattern; the eight probe graphs are byte-identical to bin-v0.8.0).
