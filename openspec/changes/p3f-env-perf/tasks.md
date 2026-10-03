## 1. Extraction

- [x] 1.1 Per-file, per-scope binding and hoisted-`var` tables under `EnvContractsFileScope`; one `any_bound` traversal for name tests and table builds.
- [x] 1.2 `switch_body` is a scope over all its cases' statements; `enum_declaration` binds its name.
- [x] 1.3 Index version `logic-21`.

## 2. Tests

- [x] 2.1 Switch-case `let` / `const` and local `enum` shadowing (fails on origin/main).
- [x] 2.2 Cached and uncached extraction give the same env facts on every JavaScript / TypeScript case.

## 3. Gates

- [x] 3.1 Extraction time before/after on probe repos and a synthetic file.
- [x] 3.2 Eight probe repos byte-identical to bin-v0.8.0 except listed shadowing fixes.
